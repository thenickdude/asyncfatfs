#include <stdint.h>

#include "asyncfatfs.h"
#include "sdcard_standard.h"
#include "fat32_standard.h"

#define AFATFS_NUM_CACHE_BLOCKS 8

#define AFATFS_SECTOR_SIZE  512
#define AFATFS_NUM_FATS    2

typedef struct afatfs_file_t {
    uint32_t filePtrOffset;
    uint32_t startCluster;
    uint32_t numClusters;
} afatfs_file_t;

typedef enum {
    AFATFS_CACHE_STATE_EMPTY,
    AFATFS_CACHE_STATE_READING,
    AFATFS_CACHE_STATE_IN_SYNC,
    AFATFS_CACHE_STATE_DIRTY,
    AFATFS_CACHE_STATE_WRITING
} AFATFS_CacheBlockState;

typedef struct afatfs_cacheBlockDescriptor_t {
    uint32_t blockIndex;
    AFATFS_CacheBlockState state;
    uint32_t age;
    bool locked;
} afatfs_cacheBlockDescriptor_t;

#define AFATFS_SUBSTATE_INITIALIZATION_READ_MBR        0
#define AFATFS_SUBSTATE_INITIALIZATION_READ_VOLUME_ID  1
#define AFATFS_SUBSTATE_INITIALIZATION_LOCATE_FREEFILE 2

typedef struct afatfs_t {
    AFATFS_Filesystem_State filesystemState;
    uint32_t substate;

    uint8_t cache[512 * AFATFS_NUM_CACHE_BLOCKS];
    afatfs_cacheBlockDescriptor_t cacheDescriptor[AFATFS_NUM_CACHE_BLOCKS];
    uint32_t cacheTimer;

    uint32_t partitionStartSector;
    uint32_t fatStartSector;
    uint32_t numClusters;
    uint32_t clusterStartSector;
    uint32_t sectorsPerCluster;
    uint32_t rootDirCluster;
} afatfs_t;

static afatfs_t afatfs;

static uint8_t *afatfs_getCacheBlockMemory(int cacheBlockIndex)
{
    return afatfs.cache + cacheBlockIndex * AFATFS_SECTOR_SIZE;
}

static void afatfs_initCacheBlock(afatfs_cacheBlockDescriptor_t *descriptor, uint32_t blockIndex, bool locked)
{
    descriptor->age = ++afatfs.cacheTimer;
    descriptor->blockIndex = blockIndex;
    descriptor->locked = locked;
    descriptor->state = AFATFS_CACHE_STATE_EMPTY;
}

/**
 * Find or allocate a cache block for the given block index on disk. First checks if the block is already cached and
 * returns that, otherwise returns the index of an empty block, otherwise returns the index of the oldest synced block,
 * otherwise returns -1 to signal failure (cache is full of dirty/locked blocks!)
 */
static int afatfs_allocateCacheBlock(uint32_t blockIndex)
{
    int allocateIndex;
    int emptyIndex = -1;

    uint32_t oldestSyncedBlockAge = 0;
    int oldestSyncedBlockIndex = -1;

    for (int i = 0; i < AFATFS_NUM_CACHE_BLOCKS; i++) {
        if (afatfs.cacheDescriptor[i].blockIndex == blockIndex) {
            afatfs.cacheDescriptor[i].age = ++afatfs.cacheTimer;
            return i;
        }

        switch (afatfs.cacheDescriptor[i].state) {
            case AFATFS_CACHE_STATE_EMPTY:
                emptyIndex = i;
            break;
            case AFATFS_CACHE_STATE_IN_SYNC:
                // This block could be evicted from the cache to make room for us since it's idle and not dirty
                if (afatfs.cacheDescriptor[i].age > oldestSyncedBlockAge && !afatfs.cacheDescriptor[i].locked) {
                    oldestSyncedBlockAge = afatfs.cacheDescriptor[i].age;
                    oldestSyncedBlockIndex = i;
                }
            break;
            default:
                ;
        }
    }

    if (emptyIndex > -1) {
        allocateIndex = emptyIndex;
    } else if (oldestSyncedBlockIndex > -1) {
        allocateIndex = oldestSyncedBlockIndex;
    } else {
        allocateIndex = -1;
    }

    if (allocateIndex > -1) {
        afatfs_initCacheBlock(&afatfs.cacheDescriptor[allocateIndex], blockIndex, false);
    }

    return allocateIndex;
}

static uint32_t afatfs_fat32BlockToPhysical(uint32_t fat32BlockIndex)
{
    return afatfs.fatStartSector + fat32BlockIndex;
}

static uint32_t afatfs_clusterNumberToPhysical(uint32_t clusterNumber)
{
    return afatfs.clusterStartSector + (clusterNumber - 2) * afatfs.sectorsPerCluster;
}

static void afatfs_sdcardReadComplete(sdcardBlockOperation_e operation, uint32_t blockIndex, uint8_t *buffer, uint32_t callbackData)
{
    for (int i = 0; i < AFATFS_NUM_CACHE_BLOCKS; i++) {
        if (afatfs.cacheDescriptor[i].blockIndex == blockIndex
                && afatfs_getCacheBlockMemory(i) == buffer) {

            if (afatfs.cacheDescriptor[i].state != AFATFS_CACHE_STATE_READING) {
                afatfs.filesystemState = AFATFS_FILESYSTEM_STATE_FATAL;
            }

            afatfs.cacheDescriptor[i].state = AFATFS_CACHE_STATE_IN_SYNC;
        }
    }
}

static AFATFSOperationStatus afatfs_readBlock(uint32_t physicalBlockIndex, uint8_t **buffer)
{
    int cacheBlockIndex = afatfs_allocateCacheBlock(physicalBlockIndex);

    if (cacheBlockIndex == -1) {
        // We don't have enough free cache to service this request right now, try again later
        return AFATFS_OPERATION_IN_PROGRESS;
    }

    switch (afatfs.cacheDescriptor[cacheBlockIndex].state) {
        case AFATFS_CACHE_STATE_READING:
            return AFATFS_OPERATION_IN_PROGRESS;
        break;

        case AFATFS_CACHE_STATE_EMPTY:
            if (sdcard_readBlock(physicalBlockIndex, afatfs_getCacheBlockMemory(cacheBlockIndex), afatfs_sdcardReadComplete)) {
                afatfs.cacheDescriptor[cacheBlockIndex].state = AFATFS_CACHE_STATE_READING;
            }
            return AFATFS_OPERATION_IN_PROGRESS;
        break;

        case AFATFS_CACHE_STATE_DIRTY:
        case AFATFS_CACHE_STATE_WRITING:
        case AFATFS_CACHE_STATE_IN_SYNC:
            *buffer = afatfs_getCacheBlockMemory(cacheBlockIndex);

            return AFATFS_OPERATION_SUCCESS;
        break;

        default:
            return AFATFS_OPERATION_ERROR;
    }
}

/**
 * Parse the details out of the given MBR block (512 bytes long). Return true if a compatible filesystem was found.
 */
static bool afatfs_parseMBR(const uint8_t *block)
{
    // Check MBR signature
    if (block[AFATFS_SECTOR_SIZE - 2] != 0x55 || block[AFATFS_SECTOR_SIZE - 2] != 0xAA)
        return false;

    mbrPartitionEntry_t *partition = block + 446;

    for (int i = 0; i < 4; i++) {
        if (partition[i]->type == MBR_PARTITION_TYPE_FAT32 || partition[i]->type == MBR_PARTITION_TYPE_FAT32_LBA) {
            afatfs.partitionStartSector = partition[i]->lbaBegin;

            return true;
        }
    }

    return false;
}

static bool isPowerOfTwo(unsigned int x)
{
    return ((x != 0) && ((x & (~x + 1)) == x));
}

static bool afatfs_parseVolumeID(uint8_t *sector)
{
    fatVolumeID_t *volume = (fatVolumeID_t *) sector;

    if (volume->bytesPerSector != AFATFS_SECTOR_SIZE || volume->numFATs != AFATFS_NUM_FATS
            || sector[510] != FAT_VOLUME_ID_SIGNATURE_1 || block[511] != FAT_VOLUME_ID_SIGNATURE_2) {
        return false;
    }

    //TODO identify (and reject) FAT16 filesystems

    afatfs.fatStartSector = afatfs.partitionStartSector + volume->reservedSectorCount;
    afatfs.clusterStartSector = afatfs.fatStartSector + AFATFS_NUM_FATS * volume->fatDescriptor.fat32.FATSize32;

    afatfs.sectorsPerCluster = volume->sectorsPerCluster;
    if (afatfs.sectorsPerCluster < 1 || afatfs.sectorsPerCluster > 128 || !isPowerOfTwo(afatfs.sectorsPerCluster)) {
        return false;
    }

    afatfs.rootDirCluster = volume->fatDescriptor.fat32.rootCluster;

    if (volume->totalSectors32 == 0)
        return false;

    afatfs.numClusters = (volume->totalSectors32 - (afatfs.clusterStartSector - afatfs.partitionStartSector)) / afatfs.sectorsPerCluster;

    return true;
}

static void afatfs_continueInit()
{
    const uint8_t *block;

    switch (afatfs.substate) {
        case AFATFS_SUBSTATE_INITIALIZATION_READ_MBR:
            if (afatfs_readBlock(0, &block) == AFATFS_OPERATION_SUCCESS) {
                if (afatfs_parseMBR(block)) {
                    afatfs.substate = AFATFS_SUBSTATE_INITIALIZATION_READ_VOLUME_ID;
                    afatfs_continueInit();
                } else {
                    afatfs.filesystemState = AFATFS_FILESYSTEM_STATE_FATAL;
                }
            }
        break;
        case AFATFS_SUBSTATE_INITIALIZATION_READ_VOLUME_ID:
            if (afatfs_readBlock(afatfs.partitionStartSector, &block) == AFATFS_OPERATION_SUCCESS) {
                if (afatfs_parseVolumeID(block)) {
                    afatfs.substate = AFATFS_SUBSTATE_INITIALIZATION_LOCATE_FREEFILE;
                } else {
                    afatfs.filesystemState = AFATFS_FILESYSTEM_STATE_FATAL;
                }
            }
        break;
    }
}

void afatfs_poll()
{
    sdcard_poll();

    switch (afatfs.filesystemState) {
        case AFATFS_FILESYSTEM_STATE_INITIALIZATION:
            afatfs_continueInit();
        break;
    }
}

AFATFS_Filesystem_State afatfs_getFilesystemState()
{
    return afatfs.filesystemState;
}

void afatfs_init()
{
    afatfs.filesystemState = AFATFS_FILESYSTEM_STATE_INITIALIZATION;
    afatfs.substate = AFATFS_SUBSTATE_INITIALIZATION_READ_MBR;

    afatfs_poll();
}

void afatfs_destroy()
{
    afatfs.filesystemState = AFATFS_FILESYSTEM_STATE_UNKNOWN;

    //TODO clear buffers
}
