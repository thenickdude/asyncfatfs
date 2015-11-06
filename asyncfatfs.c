/**
 * This is a FAT16/FAT32 filesystem for SD cards which uses asynchronous operations: The caller need never wait
 * for the SD card to be ready.
 *
 * On top of the regular FAT32 concepts, we add the idea of a super cluster. Given one FAT sector, a super cluster is
 * the series of clusters which corresponds to all of the cluster entries in that FAT sector. If files are allocated
 * on super-cluster boundaries, they will have FAT sectors which are dedicated to them and independent of all other
 * files.
 *
 * We can pre-allocate a "freefile" which is a file on disk made up of contiguous superclusters. Then when we want
 * to allocate a file on disk, we can carve it out of the freefile, and know that the clusters will be contiguous
 * without needing to read the FAT at all (the freefile's FAT is completely determined from its start cluster and file
 * size which we get from the directory entry). This allows for extremely fast append-only logging.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "asyncfatfs.h"

#include "fat_standard.h"
#include "sdcard.h"

#define AFATFS_NUM_CACHE_SECTORS 8

// FAT filesystems are allowed to differ from these parameters, but we choose not to support those weird filesystems:
#define AFATFS_SECTOR_SIZE  512
#define AFATFS_NUM_FATS     2

#define AFATFS_MAX_OPEN_FILES 2

#define AFATFS_FILES_PER_DIRECTORY_SECTOR (AFATFS_SECTOR_SIZE / sizeof(fatDirectoryEntry_t))

#define AFATFS_FAT32_FAT_ENTRIES_PER_SECTOR  (AFATFS_SECTOR_SIZE / sizeof(uint32_t))
#define AFATFS_FAT16_FAT_ENTRIES_PER_SECTOR (AFATFS_SECTOR_SIZE / sizeof(uint16_t))

#define AFATFS_SUBSTATE_INITIALIZATION_READ_MBR                  0
#define AFATFS_SUBSTATE_INITIALIZATION_READ_VOLUME_ID            1
#define AFATFS_SUBSTATE_INITIALIZATION_FREEFILE_CREATING         2
#define AFATFS_SUBSTATE_INITIALIZATION_FREEFILE_FAT_SEARCH       3
#define AFATFS_SUBSTATE_INITIALIZATION_FREEFILE_UPDATE_FAT       4
#define AFATFS_SUBSTATE_INITIALIZATION_FREEFILE_SAVE_DIR_ENTRY   5

// We will read from the file
#define AFATFS_FILE_MODE_READ             1
// We will write to the file
#define AFATFS_FILE_MODE_WRITE            2
// We will append to the file, may not be combined with the write flag
#define AFATFS_FILE_MODE_APPEND           4
// File will occupy a series of superclusters (only valid for creating new files):
#define AFATFS_FILE_MODE_CONTIGUOUS       8
// File should be created if it doesn't exist:
#define AFATFS_FILE_MODE_CREATE           16
#define AFATFS_FILE_MODE_RETAIN_DIRECTORY 32

#define AFATFS_CACHE_READ         1
#define AFATFS_CACHE_WRITE        2
#define AFATFS_CACHE_LOCK         4
#define AFATFS_CACHE_UNLOCK       8
#define AFATFS_CACHE_DISCARDABLE  16
#define AFATFS_CACHE_RETAIN       32

// Turn the largest free block on the disk into one contiguous file for efficient fragment-free allocation
#define AFATFS_USE_FREESPACE_FILE

// When allocating a freefile, leave this many clusters un-allocated for regular files to use
#define AFATFS_FREEFILE_LEAVE_CLUSTERS 100

#define AFATFS_FREESPACE_FILENAME "FREESPACE  "

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

typedef enum {
    AFATFS_CACHE_STATE_EMPTY,
    AFATFS_CACHE_STATE_READING,
    AFATFS_CACHE_STATE_IN_SYNC,
    AFATFS_CACHE_STATE_DIRTY,
    AFATFS_CACHE_STATE_WRITING
} afatfsCacheBlockState_e;

typedef enum {
    CLUSTER_SEARCH_FREE_SECTOR_AT_BEGINNING_OF_FAT_SECTOR,
    CLUSTER_SEARCH_FREE_SECTOR,
    CLUSTER_SEARCH_OCCUPIED_SECTOR,
} afatfsClusterSearchCondition_e;

enum {
    AFATFS_CREATEFILE_PHASE_INITIAL = 0,
    AFATFS_CREATEFILE_PHASE_FIND_FILE = 0,
    AFATFS_CREATEFILE_PHASE_CREATE_NEW_FILE,
    AFATFS_CREATEFILE_PHASE_SUCCESS,
    AFATFS_CREATEFILE_PHASE_FAILURE,
};

struct afatfsOperation_t;

typedef union afatfsFATSector_t {
    uint8_t *bytes;
    uint16_t *fat16;
    uint32_t *fat32;
} afatfsFATSector_t;

typedef struct afatfsCacheBlockDescriptor_t {
    uint32_t sectorIndex;
    afatfsCacheBlockState_e state;
    uint32_t lastUse;

    /*
     * The state of this block must not transition (do not flush to disk, do not discard). This is useful for a sector
     * which is currently being written to by the application (so flushing it would be a waste of time).
     */
    unsigned locked:1;

    /*
     * A counter for how many parties want this sector to be retained in memory (not discarded). If this value is
     * non-zero, the sector may be flushed to disk if dirty but must remain in the cache. This is useful if we require
     * a directory sector to be cached in order to meet our response time requirements.
     */
    unsigned retainCount:6;

    /*
     * If this block is in the In Sync state, it should be discarded from the cache in preference to other blocks.
     * This is useful for data that we don't expect to read again, e.g. data written to an append-only file. This hint
     * is overridden by the locked and retainCount flags.
     */
    unsigned discardable:1;
} afatfsCacheBlockDescriptor_t;

typedef enum {
    AFATFS_FREE_SPACE_SEARCH_PHASE_FIND_HOLE,
    AFATFS_FREE_SPACE_SEARCH_PHASE_GROW_HOLE
} afatfsFreeSpaceSearchPhase_e;

typedef struct afatfsFreeSpaceSearchState_t {
    uint32_t candidateStart;
    uint32_t candidateEnd;
    uint32_t bestGapStart;
    uint32_t bestGapLength;
    afatfsFreeSpaceSearchPhase_e phase;
} afatfsFreeSpaceSearchState_t;

typedef struct afatfsFreeSpaceFATState_t {
    uint32_t startCluster;
    uint32_t endCluster;
} afatfsFreeSpaceFATState_t;

typedef void (*afatfsCallback_t)();

typedef struct afatfsCreateFileState_t {
    afatfsFileCallback_t callback;

    uint32_t parentDirectoryCluster;

    uint8_t phase;
} afatfsCreateFileState_t;

typedef struct afatfsSeekState_t {
    afatfsFileCallback_t callback;

    uint32_t seekOffset;
} afatfsSeekState_t;

typedef enum {
    AFATFS_APPEND_SUPERCLUSTER_PHASE_INIT = 0,
    AFATFS_APPEND_SUPERCLUSTER_PHASE_UPDATE_FAT,
    AFATFS_APPEND_SUPERCLUSTER_PHASE_UPDATE_FREEFILE_DIRECTORY,
    AFATFS_APPEND_SUPERCLUSTER_PHASE_UPDATE_FILE_DIRECTORY,
} afatfsAppendSuperclusterPhase_e;

typedef struct afatfsAppendSuperclusterState_t {
    afatfsAppendSuperclusterPhase_e phase;
    uint32_t previousCluster;
    uint32_t fatRewriteStartCluster;
    uint32_t fatRewriteEndCluster;
} afatfsAppendSuperclusterState_t;

typedef enum {
    AFATFS_APPEND_FREE_CLUSTER_PHASE_INIT = 0,
    AFATFS_APPEND_FREE_CLUSTER_PHASE_FIND_FREESPACE,
    AFATFS_APPEND_FREE_CLUSTER_PHASE_UPDATE_FAT1,
    AFATFS_APPEND_FREE_CLUSTER_PHASE_UPDATE_FAT2,
    AFATFS_APPEND_FREE_CLUSTER_PHASE_UPDATE_FILE_DIRECTORY,
    AFATFS_APPEND_FREE_CLUSTER_PHASE_COMPLETE,
} afatfsAppendFreeClusterPhase_e;

typedef struct afatfsAppendFreeClusterState_t {
    afatfsAppendFreeClusterPhase_e phase;
    uint32_t previousCluster;
    uint32_t searchCluster;
} afatfsAppendFreeClusterState_t;

typedef enum {
    AFATFS_FILE_OPERATION_NONE,
    AFATFS_FILE_OPERATION_CREATE_FILE,
    AFATFS_FILE_OPERATION_SEEK, // Seek the file's cursorCluster forwards by seekOffset bytes
    AFATFS_FILE_OPERATION_CLOSE,
    AFATFS_FILE_OPERATION_APPEND_SUPERCLUSTER,
    AFATFS_FILE_OPERATION_APPEND_FREE_CLUSTER
} afatfsFileOperation_e;

typedef struct afatfsOperation_t {
    afatfsFileOperation_e operation;
    union {
        afatfsCreateFileState_t createFile;
        afatfsSeekState_t seek;
        afatfsAppendSuperclusterState_t appendSupercluster;
        afatfsAppendFreeClusterState_t appendFreeCluster;
    } state;
} afatfsFileOperation_t;

typedef struct afatfsFile_t {
    bool open;
    uint32_t cursorOffset;
    uint32_t cursorCluster, cursorPreviousCluster;

    uint8_t mode;

    afatfsDirEntryPointer_t directoryEntryPos;
    fatDirectoryEntry_t directoryEntry;

    struct afatfsOperation_t operation;
} afatfsFile_t;

typedef struct afatfs_t {
    fatFilesystemType_e filesystemType;

    afatfsFilesystemState_e filesystemState;
    uint32_t substate;

    // State used during FS initialisation where only one member of the union is used at a time
    union {
#ifdef AFATFS_USE_FREESPACE_FILE
        afatfsFreeSpaceSearchState_t freeSpaceSearch;
        afatfsFreeSpaceFATState_t freeSpaceFAT;
#endif
    } initState;

    uint8_t cache[AFATFS_SECTOR_SIZE * AFATFS_NUM_CACHE_SECTORS];
    afatfsCacheBlockDescriptor_t cacheDescriptor[AFATFS_NUM_CACHE_SECTORS];
    uint32_t cacheTimer;
    int cacheDirtyEntries; // The number of cache entries in the AFATFS_CACHE_STATE_DIRTY state

    afatfsFile_t openFiles[AFATFS_MAX_OPEN_FILES];

#ifdef AFATFS_USE_FREESPACE_FILE
    afatfsFile_t freeFile;
#endif

    uint32_t currentDirectoryCluster;
    uint32_t currentDirectorySector;

    uint32_t partitionStartSector;

    uint32_t fatStartSector; // The first sector of the first FAT
    uint32_t fatSectors;     // The size in sectors of a single FAT

    /*
     * Number of clusters available for storing user data. Note that clusters are numbered starting from 2, so the
     * index of the last cluster on the volume is numClusters + 1 !!!
     */
    uint32_t numClusters;
    uint32_t clusterStartSector;
    uint32_t sectorsPerCluster;

    uint32_t rootDirectoryCluster; // Present on FAT32 and set to zero for FAT16
    uint32_t rootDirectorySectors;
} afatfs_t;

static afatfs_t afatfs;

static void afatfs_fileOperationContinue(afatfsFile_t *file);
static afatfsOperationStatus_e afatfs_writeContiguousDedicatedFATChain(uint32_t *startCluster, uint32_t endCluster);

static uint32_t roundUpTo(uint32_t value, uint32_t rounding)
{
    uint32_t remainder = value % rounding;

    if (remainder) {
        value += rounding - remainder;
    }

    return value;
}

static bool isPowerOfTwo(unsigned int x)
{
    return ((x != 0) && ((x & (~x + 1)) == x));
}

static bool afatfs_assert(bool condition)
{
    if (!condition) {
        afatfs.filesystemState = AFATFS_FILESYSTEM_STATE_FATAL;
    }

    return condition;
}

/**
 * The number of FAT table entries that fit within one AFATFS sector size.
 */
static uint32_t afatfs_fatEntriesPerSector()
{
    return afatfs.filesystemType == FAT_FILESYSTEM_TYPE_FAT32 ? AFATFS_FAT32_FAT_ENTRIES_PER_SECTOR : AFATFS_FAT16_FAT_ENTRIES_PER_SECTOR;
}

/**
 * Size of a FAT cluster in bytes
 */
 static uint32_t afatfs_clusterSize()
{
    return afatfs.sectorsPerCluster * AFATFS_SECTOR_SIZE;
}

/**
 * Size of a AFATFS supercluster in bytes
 */
static uint32_t afatfs_superClusterSize()
{
    return afatfs_fatEntriesPerSector() * afatfs_clusterSize();
}

static uint8_t *afatfs_cacheSectorGetMemory(int cacheSectorIndex)
{
    return afatfs.cache + cacheSectorIndex * AFATFS_SECTOR_SIZE;
}

static afatfsCacheBlockDescriptor_t* afatfs_getCacheDescriptorForBuffer(uint8_t *memory)
{
    int index = (memory - afatfs.cache) / AFATFS_SECTOR_SIZE;

    if (afatfs_assert(index >= 0 && index < AFATFS_NUM_CACHE_SECTORS)) {
        return afatfs.cacheDescriptor + index;
    } else {
        return NULL;
    }
}

/**
 * Mark the cached sector that the given memory pointer lies inside as dirty.
 */
static void afatfs_cacheSectorMarkDirty(uint8_t *memory)
{
    afatfsCacheBlockDescriptor_t *descriptor = afatfs_getCacheDescriptorForBuffer(memory);

    if (descriptor && descriptor->state != AFATFS_CACHE_STATE_DIRTY) {
        descriptor->state = AFATFS_CACHE_STATE_DIRTY;
        afatfs.cacheDirtyEntries++;
    }
}

static void afatfs_cacheSectorInit(afatfsCacheBlockDescriptor_t *descriptor, uint32_t sectorIndex, bool locked)
{
    descriptor->lastUse = ++afatfs.cacheTimer;
    descriptor->sectorIndex = sectorIndex;
    descriptor->locked = locked;
    descriptor->discardable = 0;
    descriptor->state = AFATFS_CACHE_STATE_EMPTY;
}

static void afatfs_sdcardReadComplete(sdcardBlockOperation_e operation, uint32_t sectorIndex, uint8_t *buffer, uint32_t callbackData)
{
    (void) operation;
    (void) callbackData;

    for (int i = 0; i < AFATFS_NUM_CACHE_SECTORS; i++) {
        if (afatfs.cacheDescriptor[i].state != AFATFS_CACHE_STATE_EMPTY
            && afatfs.cacheDescriptor[i].sectorIndex == sectorIndex
        ) {
            afatfs_assert(afatfs_cacheSectorGetMemory(i) == buffer && afatfs.cacheDescriptor[i].state == AFATFS_CACHE_STATE_READING);

            afatfs.cacheDescriptor[i].state = AFATFS_CACHE_STATE_IN_SYNC;
            break;
        }
    }
}

static void afatfs_sdcardWriteComplete(sdcardBlockOperation_e operation, uint32_t sectorIndex, uint8_t *buffer, uint32_t callbackData)
{
    (void) operation;
    (void) callbackData;

    for (int i = 0; i < AFATFS_NUM_CACHE_SECTORS; i++) {
        /* Keep in mind that someone may have marked the sector as dirty after writing had already begun. In this case we must leave
         * it marked as dirty because those modifications may have been made too late to make it to the disk!
         */
        if (afatfs.cacheDescriptor[i].state == AFATFS_CACHE_STATE_WRITING
            && afatfs.cacheDescriptor[i].sectorIndex == sectorIndex
        ) {
            afatfs_assert(afatfs_cacheSectorGetMemory(i) == buffer);

            afatfs.cacheDescriptor[i].state = AFATFS_CACHE_STATE_IN_SYNC;
            break;
        }
    }
}

/**
 * Find a sector in the cache which corresponds to the given physical sector index, or NULL if the sector isn't
 * cached. Note that the cached sector could be in any state including completely empty.
 */
static afatfsCacheBlockDescriptor_t* afatfs_findCacheSector(uint32_t sectorIndex)
{
    for (int i = 0; i < AFATFS_NUM_CACHE_SECTORS; i++) {
        if (afatfs.cacheDescriptor[i].sectorIndex == sectorIndex) {
            return &afatfs.cacheDescriptor[i];
        }
    }

    return NULL;
}

/**
 * Find or allocate a cache sector for the given sector index on disk. Returns a block which matches one of these
 * conditions (in descending order of preference):
 *
 * - The requested sector that already exists in the cache
 * - The index of an empty sector
 * - The index of a synced discardable sector
 * - The index of the oldest synced sector
 *
 * Otherwise it returns -1 to signal failure (cache is full!)
 */
static int afatfs_allocateCacheSector(uint32_t sectorIndex)
{
    int allocateIndex;
    int emptyIndex = -1, discardableIndex = -1;

    uint32_t oldestSyncedSectorLastUse = 0xFFFFFFFF;
    int oldestSyncedSectorIndex = -1;

    for (int i = 0; i < AFATFS_NUM_CACHE_SECTORS; i++) {
        if (afatfs.cacheDescriptor[i].sectorIndex == sectorIndex) {
            /*
             * If the sector is actually empty then do a complete re-init of it just like the standard
             * empty case. (Sectors marked as empty should be treated as if they don't have a block index assigned)
             */
            if (afatfs.cacheDescriptor[i].state == AFATFS_CACHE_STATE_EMPTY) {
                emptyIndex = i;
                break;
            }

            // Bump the last access time
            afatfs.cacheDescriptor[i].lastUse = ++afatfs.cacheTimer;
            return i;
        }

        switch (afatfs.cacheDescriptor[i].state) {
            case AFATFS_CACHE_STATE_EMPTY:
                emptyIndex = i;
            break;
            case AFATFS_CACHE_STATE_IN_SYNC:
                if (!afatfs.cacheDescriptor[i].locked && afatfs.cacheDescriptor[i].retainCount == 0) {
                    if (afatfs.cacheDescriptor[i].discardable) {
                        discardableIndex = i;
                    } else if (afatfs.cacheDescriptor[i].lastUse < oldestSyncedSectorLastUse) {
                        // This block could be evicted from the cache to make room for us since it's idle and not dirty
                        oldestSyncedSectorLastUse = afatfs.cacheDescriptor[i].lastUse;
                        oldestSyncedSectorIndex = i;
                    }
                }
            break;
            default:
                ;
        }
    }

    if (emptyIndex > -1) {
        allocateIndex = emptyIndex;
    } else if (discardableIndex > -1) {
        allocateIndex = discardableIndex;
    } else if (oldestSyncedSectorIndex > -1) {
        allocateIndex = oldestSyncedSectorIndex;
    } else {
        allocateIndex = -1;
    }

    if (allocateIndex > -1) {
        afatfs_cacheSectorInit(&afatfs.cacheDescriptor[allocateIndex], sectorIndex, false);
    }

    return allocateIndex;
}

/**
 * Attempt to flush dirty cache pages out to the card, returning true if all flushable data has been flushed.
 */
bool afatfs_flush()
{
    if (afatfs.cacheDirtyEntries > 0) {
        for (int i = 0; i < AFATFS_NUM_CACHE_SECTORS; i++) {
            if (afatfs.cacheDescriptor[i].state == AFATFS_CACHE_STATE_DIRTY && !afatfs.cacheDescriptor[i].locked) {
                if (sdcard_writeBlock(afatfs.cacheDescriptor[i].sectorIndex, afatfs_cacheSectorGetMemory(i), afatfs_sdcardWriteComplete, 0)) {
                    afatfs.cacheDescriptor[i].state = AFATFS_CACHE_STATE_WRITING;
                    afatfs.cacheDirtyEntries--;
                }
                return false;
            }
        }
    }

    return true;
}

/**
 * Get the physical sector number that corresponds to the FAT sector of the given fatSectorIndex within the given
 * FAT (fatIndex may be 0 or 1). (0, 0) gives the first sector of the first FAT.
 */
static uint32_t afatfs_fatSectorToPhysical(int fatIndex, uint32_t fatSectorIndex)
{
    return afatfs.fatStartSector + (fatIndex ? afatfs.fatSectors : 0) + fatSectorIndex;
}

static uint32_t afatfs_fileClusterToPhysical(uint32_t clusterNumber, uint32_t sectorIndex)
{
    return afatfs.clusterStartSector + (clusterNumber - 2) * afatfs.sectorsPerCluster + sectorIndex;
}

/**
 * Get a cache entry for the given sector that is suitable for write only (no read!)
 *
 * lock        - True if the sector should not be flushed to disk yet, false to clear the lock.
 * discardable - Set to true as a hint that this sector needn't be retained in cache after writing.
 */
static afatfsOperationStatus_e afatfs_cacheSector(uint32_t physicalSectorIndex, uint8_t **buffer, uint8_t sectorFlags)
{
    int cacheSectorIndex = afatfs_allocateCacheSector(physicalSectorIndex);

    if (cacheSectorIndex == -1) {
        // We don't have enough free cache to service this request right now, try again later
        return AFATFS_OPERATION_IN_PROGRESS;
    }

    switch (afatfs.cacheDescriptor[cacheSectorIndex].state) {
        case AFATFS_CACHE_STATE_READING:
            return AFATFS_OPERATION_IN_PROGRESS;
        break;

        case AFATFS_CACHE_STATE_EMPTY:
            if ((sectorFlags & AFATFS_CACHE_READ) != 0) {
                if (sdcard_readBlock(physicalSectorIndex, afatfs_cacheSectorGetMemory(cacheSectorIndex), afatfs_sdcardReadComplete, 0)) {
                    afatfs.cacheDescriptor[cacheSectorIndex].state = AFATFS_CACHE_STATE_READING;
                }
                return AFATFS_OPERATION_IN_PROGRESS;
            }

            // We only get to decide if it is discardable if we're the ones who fill it
            afatfs.cacheDescriptor[cacheSectorIndex].discardable = (sectorFlags & AFATFS_CACHE_DISCARDABLE) != 0 ? 1 : 0;

            // Fall through

        case AFATFS_CACHE_STATE_WRITING:
        case AFATFS_CACHE_STATE_IN_SYNC:
            if ((sectorFlags & AFATFS_CACHE_WRITE) != 0) {
                afatfs.cacheDescriptor[cacheSectorIndex].state = AFATFS_CACHE_STATE_DIRTY;
                afatfs.cacheDirtyEntries++;
            }
            // Fall through

        case AFATFS_CACHE_STATE_DIRTY:
            if ((sectorFlags & AFATFS_CACHE_LOCK) != 0) {
                afatfs.cacheDescriptor[cacheSectorIndex].locked = 1;
            }
            if ((sectorFlags & AFATFS_CACHE_UNLOCK) != 0) {
                afatfs.cacheDescriptor[cacheSectorIndex].locked = 0;
            }
            if ((sectorFlags & AFATFS_CACHE_RETAIN) != 0) {
                afatfs.cacheDescriptor[cacheSectorIndex].retainCount++;
            }

            *buffer = afatfs_cacheSectorGetMemory(cacheSectorIndex);

            return AFATFS_OPERATION_SUCCESS;
        break;

        default:
            return AFATFS_OPERATION_FAILURE;
    }
}

/**
 * Parse the details out of the given MBR sector (512 bytes long). Return true if a compatible filesystem was found.
 */
static bool afatfs_parseMBR(const uint8_t *sector)
{
    // Check MBR signature
    if (sector[AFATFS_SECTOR_SIZE - 2] != 0x55 || sector[AFATFS_SECTOR_SIZE - 1] != 0xAA)
        return false;

    mbrPartitionEntry_t *partition = (mbrPartitionEntry_t *) (sector + 446);

    for (int i = 0; i < 4; i++) {
        if (partition[i].type == MBR_PARTITION_TYPE_FAT32 || partition[i].type == MBR_PARTITION_TYPE_FAT32_LBA) {
            afatfs.partitionStartSector = partition[i].lbaBegin;

            return true;
        }
    }

    return false;
}

static bool afatfs_parseVolumeID(const uint8_t *sector)
{
    fatVolumeID_t *volume = (fatVolumeID_t *) sector;

    if (volume->bytesPerSector != AFATFS_SECTOR_SIZE || volume->numFATs != AFATFS_NUM_FATS
            || sector[510] != FAT_VOLUME_ID_SIGNATURE_1 || sector[511] != FAT_VOLUME_ID_SIGNATURE_2) {
        return false;
    }

    afatfs.fatStartSector = afatfs.partitionStartSector + volume->reservedSectorCount;

    afatfs.sectorsPerCluster = volume->sectorsPerCluster;
    if (afatfs.sectorsPerCluster < 1 || afatfs.sectorsPerCluster > 128 || !isPowerOfTwo(afatfs.sectorsPerCluster)) {
        return false;
    }

    afatfs.fatSectors = volume->FATSize16 != 0 ? volume->FATSize16 : volume->fatDescriptor.fat32.FATSize32;

    // Always zero on FAT32 since rootEntryCount is always zero (this is non-zero on FAT16)
    afatfs.rootDirectorySectors = ((volume->rootEntryCount * FAT_DIRECTORY_ENTRY_SIZE) + (volume->bytesPerSector - 1)) / volume->bytesPerSector;
    uint32_t totalSectors = volume->totalSectors16 != 0 ? volume->totalSectors16 : volume->totalSectors32;
    uint32_t dataSectors = totalSectors - (volume->reservedSectorCount + (AFATFS_NUM_FATS * afatfs.fatSectors) + afatfs.rootDirectorySectors);

    afatfs.numClusters = dataSectors / volume->sectorsPerCluster;

    if (afatfs.numClusters <= FAT12_MAX_CLUSTERS) {
        afatfs.filesystemType = FAT_FILESYSTEM_TYPE_FAT12;
        afatfs.filesystemState = AFATFS_FILESYSTEM_STATE_FATAL;

        return false; // FAT12 is not a supported filesystem
    } else if (afatfs.numClusters <= FAT16_MAX_CLUSTERS) {
        afatfs.filesystemType = FAT_FILESYSTEM_TYPE_FAT16;
    } else {
        afatfs.filesystemType = FAT_FILESYSTEM_TYPE_FAT32;
    }

    uint32_t endOfFATs = afatfs.fatStartSector + AFATFS_NUM_FATS * afatfs.fatSectors;

    if (afatfs.filesystemType == FAT_FILESYSTEM_TYPE_FAT32) {
        afatfs.rootDirectoryCluster = volume->fatDescriptor.fat32.rootCluster;
    } else {
        // FAT16 doesn't store the root directory in clusters
        afatfs.rootDirectoryCluster = 0;
    }

    afatfs.currentDirectoryCluster = afatfs.rootDirectoryCluster;
    afatfs.currentDirectorySector = 0;

    afatfs.clusterStartSector = endOfFATs + afatfs.rootDirectorySectors;

    return true;
}

/**
 * Get the position of the dirEntryPos in the FAT for the cluster with the given number.
 */
static void afatfs_getFATPositionForCluster(uint32_t cluster, uint32_t *fatSectorIndex, uint32_t *fatSectorEntryIndex)
{
    if (afatfs.filesystemType == FAT_FILESYSTEM_TYPE_FAT16) {
        // There are AFATFS_SECTOR_SIZE / sizeof(uint16_t) entries per FAT16 sector
        *fatSectorIndex = (cluster & 0x0FFFFFFF) >> 8;
        *fatSectorEntryIndex = cluster & 0xFF;
    } else {
        // There are AFATFS_SECTOR_SIZE / sizeof(uint32_t) entries per FAT32 sector
        *fatSectorIndex = (cluster & 0x0FFFFFFF) >> 7;
        *fatSectorEntryIndex = cluster & 0x7F;
    }
}

/**
 * Look up the FAT to find out which cluster follows the one with the given number and store it into *nextCluster.
 *
 * Use fat_isFreeSpace() and fat_isEndOfChainMarker() on nextCluster to distinguish those special values from regular
 * cluster numbers.
 *
 * Returns:
 *     AFATFS_OPERATION_IN_PROGRESS - FS is busy right now, call again later
 *     AFATFS_OPERATION_SUCCESS     - On success
 */
static afatfsOperationStatus_e afatfs_FATGetNextCluster(int fatIndex, uint32_t cluster, uint32_t *nextCluster)
{
    uint32_t fatSectorIndex, fatSectorEntryIndex;
    afatfsFATSector_t sector;

    afatfs_getFATPositionForCluster(cluster, &fatSectorIndex, &fatSectorEntryIndex);

    afatfsOperationStatus_e result =  afatfs_cacheSector(afatfs_fatSectorToPhysical(fatIndex, fatSectorIndex), &sector.bytes, AFATFS_CACHE_READ);

    if (result == AFATFS_OPERATION_SUCCESS) {
        if (afatfs.filesystemType == FAT_FILESYSTEM_TYPE_FAT16) {
            *nextCluster = sector.fat16[fatSectorEntryIndex];
        } else {
            *nextCluster = fat32_decodeClusterNumber(sector.fat32[fatSectorEntryIndex]);
        }
    }

    return result;
}

/**
 * Set the cluster number that follows the given cluster. Pass 0xFFFFFFFF for nextCluster to terminate the FAT chain.
 *
 * Returns:
 *     AFATFS_OPERATION_SUCCESS - On success
 *     AFATFS_OPERATION_IN_PROGRESS - Card is busy, call again later
 *     AFATFS_OPERATION_FAILURE - If something goes wrong
 */
static afatfsOperationStatus_e afatfs_FATSetNextCluster(uint32_t startCluster, uint32_t nextCluster)
{
    afatfsFATSector_t sector;
    uint32_t fatSectorIndex, fatSectorEntryIndex, fatPhysicalSector;
    afatfsOperationStatus_e result;

    afatfs_getFATPositionForCluster(startCluster, &fatSectorIndex, &fatSectorEntryIndex);

    fatPhysicalSector = afatfs_fatSectorToPhysical(0, fatSectorIndex);

    result = afatfs_cacheSector(fatPhysicalSector, &sector.bytes, AFATFS_CACHE_READ | AFATFS_CACHE_WRITE);

    if (result != AFATFS_OPERATION_SUCCESS)
        return result;

    if (afatfs.filesystemType == FAT_FILESYSTEM_TYPE_FAT16) {
        sector.fat16[fatSectorEntryIndex] = nextCluster;
    } else {
        sector.fat32[fatSectorEntryIndex] = nextCluster;
    }

    return AFATFS_OPERATION_SUCCESS;
}

/**
 * Starting from and including the given cluster number, find the number of the first cluster which matches the given
 * condition.
 *
 * Condition:
 *     CLUSTER_SEARCH_FREE_SECTOR_AT_BEGINNING_OF_FAT_SECTOR - Find a cluster marked as free in the FAT which lies at the
 *     beginning of its FAT sector. The passed initial search 'cluster' must correspond to the first entry of a FAT sector.
 *     CLUSTER_SEARCH_OCCUPIED_SECTOR - Find a cluster marked as occupied in the FAT.
 */
static afatfsOperationStatus_e afatfs_findClusterWithCondition(afatfsClusterSearchCondition_e condition, uint32_t *cluster)
{
    afatfsFATSector_t sector;
    uint32_t fatSectorIndex, fatSectorEntryIndex;

    uint32_t fatEntriesPerSector = afatfs_fatEntriesPerSector();
    bool lookingForFree = condition == CLUSTER_SEARCH_FREE_SECTOR_AT_BEGINNING_OF_FAT_SECTOR || condition == CLUSTER_SEARCH_FREE_SECTOR;

    int jump;

    // Get the FAT entry which corresponds to this cluster so we can begin our search there
    afatfs_getFATPositionForCluster(*cluster, &fatSectorIndex, &fatSectorEntryIndex);

    switch (condition) {
        case CLUSTER_SEARCH_FREE_SECTOR_AT_BEGINNING_OF_FAT_SECTOR:
            jump = fatEntriesPerSector;

            // We're supposed to call this routine with the cluster properly aligned
            afatfs_assert(fatSectorEntryIndex == 0);
        break;
        case CLUSTER_SEARCH_OCCUPIED_SECTOR:
        case CLUSTER_SEARCH_FREE_SECTOR:
            jump = 1;
        break;
    }

    while (*cluster < afatfs.numClusters + FAT_SMALLEST_LEGAL_CLUSTER_NUMBER) {

#ifdef AFATFS_USE_FREESPACE_FILE
        // If we're looking inside the freefile, we won't find any free clusters! Skip it!
        if (*cluster == (uint32_t) ((afatfs.freeFile.directoryEntry.firstClusterHigh << 16) + afatfs.freeFile.directoryEntry.firstClusterLow)) {
            // Add at least one so we definitely make progress (in the case that the freefile is empty)
            *cluster += MAX((afatfs.freeFile.directoryEntry.fileSize + afatfs_clusterSize() - 1) / afatfs_clusterSize(), 1);
            continue; // Go back to check that the new cluster number is valid
        }
#endif

        afatfsOperationStatus_e result = afatfs_cacheSector(afatfs_fatSectorToPhysical(0, fatSectorIndex), &sector.bytes, AFATFS_CACHE_READ | AFATFS_CACHE_DISCARDABLE);

        if (result == AFATFS_OPERATION_SUCCESS) {
            do {
                uint32_t clusterNumber;

                switch (afatfs.filesystemType) {
                    case FAT_FILESYSTEM_TYPE_FAT16:
                        clusterNumber = sector.fat16[fatSectorEntryIndex];
                    break;
                    case FAT_FILESYSTEM_TYPE_FAT32:
                        clusterNumber = fat32_decodeClusterNumber(sector.fat32[fatSectorEntryIndex]);
                    break;
                    default:
                        return AFATFS_OPERATION_FAILURE;
                }

                if (fat_isFreeSpace(clusterNumber) == lookingForFree) {
                    return AFATFS_OPERATION_SUCCESS;
                }

                (*cluster) += jump;
                fatSectorEntryIndex += jump;
            } while (fatSectorEntryIndex < fatEntriesPerSector);

            if (fatSectorEntryIndex == fatEntriesPerSector) {
                // Move on to the next FAT sector
                fatSectorIndex++;
                fatSectorEntryIndex = 0;
            }
        } else {
            return result;
        }
    }

    // We looked at every available cluster and didn't find one matching the condition
    return AFATFS_OPERATION_FAILURE;
}

static uint32_t afatfs_directorySectorToPhysical(uint32_t clusterNumber, uint32_t sectorNumber)
{
    if (clusterNumber == 0) {
        // FAT16 root directory
        return afatfs.fatStartSector + AFATFS_NUM_FATS * afatfs.fatSectors + sectorNumber;
    } else {
        return afatfs_fileClusterToPhysical(clusterNumber, sectorNumber);
    }
}

/**
 * Attempt to advance the directory pointer `finder` to the next entry in the directory, and if the directory is
 * not finished (marked by the finder being set to finished) the *entry pointer is updated to point inside the cached
 * FAT sector at the position of the fatDirectoryEntry_t. This cache could evaporate soon, so copy the entry away if
 * you need it!
 *
 * Returns AFATFS_OPERATION_SUCCESS on success and loads the next entry's details into the entry.
 * Returns AFATFS_OPERATION_IN_PROGRESS when the disk is busy. The pointer is not advanced, call again later to retry.
 */
static afatfsOperationStatus_e afatfs_findNext(afatfsDirEntryPointer_t *finder, fatDirectoryEntry_t **entry)
{
    afatfsOperationStatus_e result;
    uint8_t *sector;

    if (finder->finished) {
        return AFATFS_OPERATION_FAILURE;
    }

    // Is this the last entry in the sector? If so we need to advance to the next sector
    if (finder->entryIndex == AFATFS_FILES_PER_DIRECTORY_SECTOR - 1) {

        /* Is this the last entry in the cluster, and this isn't a FAT16 root directory, which doesn't use clusters?
         * Advance to the next cluster.
         */
        if (finder->sectorNumber == afatfs.sectorsPerCluster - 1 && finder->clusterNumber != 0) {
            uint32_t nextCluster;

            result = afatfs_FATGetNextCluster(0, finder->clusterNumber, &nextCluster);

            if (result == AFATFS_OPERATION_SUCCESS) {
                if ((afatfs.filesystemType == FAT_FILESYSTEM_TYPE_FAT32 && fat32_isEndOfChainMarker(nextCluster))
                        || (afatfs.filesystemType == FAT_FILESYSTEM_TYPE_FAT16 && fat16_isEndOfChainMarker(nextCluster))) {
                    finder->finished = true;
                    return result;
                }

                finder->clusterNumber = nextCluster;
                finder->sectorNumber = 0;
                finder->entryIndex = -1; // Fall through to the regular findNext case to advance this to 0
            } else {
                return result;
            }
        } else {
            finder->sectorNumber++;
            finder->entryIndex = -1;

            if (finder->clusterNumber == 0 && finder->sectorNumber == afatfs.rootDirectorySectors) {
                finder->finished = true;
                return AFATFS_OPERATION_SUCCESS;
            }
        }
    }

    result = afatfs_cacheSector(afatfs_directorySectorToPhysical(finder->clusterNumber, finder->sectorNumber), &sector, AFATFS_CACHE_READ);

    if (result == AFATFS_OPERATION_SUCCESS) {
        finder->entryIndex++;
        finder->finished = false;
        *entry = ((fatDirectoryEntry_t*) sector) + finder->entryIndex;
    }

    return result;
}

/**
 * Initialise the finder so that the first call with it to findNext() will return the first file in the
 * given directoryCluster.
 *
 * To search in the FAT16 root directory, pass 0 for directoryCluster.
 */
static void afatfs_findFirst(afatfsDirEntryPointer_t *finder, uint32_t directoryCluster)
{
    finder->clusterNumber = directoryCluster;
    finder->sectorNumber = 0;
    finder->entryIndex = -1;
    finder->finished = false;
}

/**
 * Allocate space for a new directory entry to be written, store the position of that entry in dirEntry, and set
 * the *entry pointer to point to it within the cached FAT sector. This pointer's lifetime is only as good as the life
 * of the cache, so don't dawdle.
 *
 * Before the first call to this function, call afatfs_findFirst() on the dirEntry to initialize it for the directory
 * you want to create the file inside.
 *
 * The FAT sector in the cache is marked as dirty so any changes written through to the entry will be flushed out
 * in the next poll cycle.
 */
static afatfsOperationStatus_e afatfs_allocateDirectoryEntry(afatfsDirEntryPointer_t *dirEntry, fatDirectoryEntry_t **entry)
{
    afatfsOperationStatus_e result;

    while ((result = afatfs_findNext(dirEntry, entry)) == AFATFS_OPERATION_SUCCESS) {
        if (dirEntry->finished) {
            // We got to the end of the directory chain without finding a free space to store our file.
            // We need to extend the chain

            // TODO
            afatfs_assert(false);

            return AFATFS_OPERATION_FAILURE;
        } else if (fat_isDirectoryEntryEmpty(*entry) || fat_isDirectoryEntryTerminator(*entry)) {
            afatfs_cacheSectorMarkDirty((uint8_t*) *entry);

            return AFATFS_OPERATION_SUCCESS;
        }
    }

    return result;
}

/**
 * Attempt to write the given directory `entry` to the position noted in the given `dirEntry` pointer. Returns:
 *
 * AFATFS_OPERATION_SUCCESS - The directory entry has been stored into the directory sector in cache.
 * AFATFS_OPERATION_IN_PROGRESS - Cache is too busy, retry later
 * AFATFS_OPERATION_FAILURE - If something goes wrong.
 */
static afatfsOperationStatus_e afatfs_saveDirectoryEntry(afatfsFilePtr_t file)
{
    uint8_t *sector;
    afatfsOperationStatus_e result;

    result = afatfs_cacheSector(afatfs_directorySectorToPhysical(file->directoryEntryPos.clusterNumber, file->directoryEntryPos.sectorNumber), &sector, AFATFS_CACHE_READ | AFATFS_CACHE_WRITE);

    if (result == AFATFS_OPERATION_SUCCESS) {
        memcpy(sector + file->directoryEntryPos.entryIndex * FAT_DIRECTORY_ENTRY_SIZE, &file->directoryEntry, FAT_DIRECTORY_ENTRY_SIZE);
    }

    return result;
}

void afatfs_seekContinue(afatfsFile_t *file)
{
    afatfsSeekState_t *opState = &file->operation.state.seek;
    uint32_t clusterSizeBytes = afatfs_clusterSize();
    uint32_t offsetInCluster = file->cursorOffset % afatfs_clusterSize();

    afatfsOperationStatus_e status;

    // Keep advancing the cursor cluster forwards to consume seekOffset
    while (file->cursorCluster != 0 && offsetInCluster + opState->seekOffset >= clusterSizeBytes) {
        uint32_t nextCluster;

        status = afatfs_FATGetNextCluster(0, file->cursorCluster, &nextCluster);

        if (status == AFATFS_OPERATION_SUCCESS) {
            // Seek to the beginning of the next cluster
            uint32_t bytesToSeek = clusterSizeBytes - offsetInCluster;

            file->cursorPreviousCluster = file->cursorCluster;
            file->cursorCluster = nextCluster;

            file->cursorOffset += bytesToSeek;
            opState->seekOffset -= bytesToSeek;
            offsetInCluster = 0;
        } else {
            // Try again later
            return;
        }
    }

    // If we didn't already hit the end of the file, add any remaining offset needed inside the cluster
    if (file->cursorCluster != 0) {
        file->cursorOffset += opState->seekOffset;
        opState->seekOffset = 0;
    }

    file->operation.operation = AFATFS_FILE_OPERATION_NONE;
    opState->callback(file);
}

/**
 * Seek the file pointer forwards by offset bytes.
 */
void afatfs_fseekInternal(afatfsFilePtr_t file, uint32_t offset, afatfsFileCallback_t callback)
{
    file->operation.operation = AFATFS_FILE_OPERATION_SEEK;

    file->operation.state.seek.callback = callback;
    file->operation.state.seek.seekOffset = offset;

    afatfs_seekContinue(file);
}


static afatfsFilePtr_t afatfs_allocateFileHandle()
{
    for (int i = 0; i < AFATFS_MAX_OPEN_FILES; i++) {
        if (!afatfs.openFiles[i].open) {
            return &afatfs.openFiles[i];
        }
    }

    return NULL;
}

void afatfs_createFileInternalContinue(afatfsFile_t *file)
{
    afatfsCreateFileState_t *opState = &file->operation.state.createFile;
    fatDirectoryEntry_t *entry;
    afatfsOperationStatus_e status;

    doMore:

    switch (opState->phase) {
        case AFATFS_CREATEFILE_PHASE_FIND_FILE:
            status = afatfs_findNext(&file->directoryEntryPos, &entry);

            if (status == AFATFS_OPERATION_SUCCESS) {
                if (file->directoryEntryPos.finished) {
                    if (file->mode & AFATFS_FILE_MODE_CREATE) {
                        // The file didn't already exist, so we can create it. Allocate a new directory entry
                        afatfs_findFirst(&file->directoryEntryPos, opState->parentDirectoryCluster);

                        opState->phase = AFATFS_CREATEFILE_PHASE_CREATE_NEW_FILE;
                        goto doMore;
                    } else {
                        // File not found.
                        opState->phase = AFATFS_CREATEFILE_PHASE_FAILURE;
                        goto doMore;
                    }
                } else if (strncmp(entry->filename, (char*) file->directoryEntry.filename, FAT_FILENAME_LENGTH) == 0) {
                    // We found a file with this name!
                    memcpy(&file->directoryEntry, entry, sizeof(file->directoryEntry));

                    opState->phase = AFATFS_CREATEFILE_PHASE_SUCCESS;
                    goto doMore;
                }
            } else if (status == AFATFS_OPERATION_FAILURE) {
                opState->phase = AFATFS_CREATEFILE_PHASE_FAILURE;
                goto doMore;
            }
        break;
        case AFATFS_CREATEFILE_PHASE_CREATE_NEW_FILE:
            status = afatfs_allocateDirectoryEntry(&file->directoryEntryPos, &entry);

            if (status == AFATFS_OPERATION_SUCCESS) {
                memcpy(entry, &file->directoryEntry, sizeof(file->directoryEntry));

                opState->phase = AFATFS_CREATEFILE_PHASE_SUCCESS;
                goto doMore;
            } else if (status == AFATFS_OPERATION_FAILURE) {
                opState->phase = AFATFS_CREATEFILE_PHASE_FAILURE;
                goto doMore;
            }
        break;
        case AFATFS_CREATEFILE_PHASE_SUCCESS:
            if ((file->mode & AFATFS_FILE_MODE_RETAIN_DIRECTORY) != 0) {
                // For this high performance file type, we require the directory entry to be retained in the cache at all times
                uint8_t *directorySector;

                status = afatfs_cacheSector(
                    afatfs_directorySectorToPhysical(file->directoryEntryPos.clusterNumber, file->directoryEntryPos.sectorNumber),
                    &directorySector,
                    AFATFS_CACHE_READ | AFATFS_CACHE_RETAIN
                );

                if (status != AFATFS_OPERATION_SUCCESS) {
                    // Retry next time
                    break;
                }
            }

            file->open = true;

            file->cursorCluster = (file->directoryEntry.firstClusterHigh << 16) | file->directoryEntry.firstClusterLow;
            file->cursorPreviousCluster = 0;
            file->cursorOffset = 0;

            if (file->directoryEntry.fileSize > 0) {
                // We can't guarantee that the existing file contents are contiguous
                file->mode &= ~AFATFS_FILE_MODE_CONTIGUOUS;

                // Seek to the end of the file if it is in append mode
                if (file->mode & AFATFS_FILE_MODE_APPEND) {
                    // This replaces our open file operation
                    afatfs_fseekInternal(file, file->directoryEntry.fileSize, file->operation.state.createFile.callback);
                    return;
                }
            }

            file->operation.operation = AFATFS_FILE_OPERATION_NONE;
            opState->callback(file);
        break;
        case AFATFS_CREATEFILE_PHASE_FAILURE:
            file->operation.operation = AFATFS_FILE_OPERATION_NONE;
            opState->callback(NULL);
        break;
    }
}

/**
 * Open (or create) a file in the CWD with the given filename.
 *
 * name             - Filename in "name.ext" format. No path.
 * attrib           - FAT file attributes to give the file (if created)
 * fileMode         - Bitset of AFATFS_FILE_MODE_* constants. Include AFATFS_FILE_MODE_CREATE to create the file if
 *                    it does not exist.
 * internalCallback - Called when the operation is complete
 * userCallback     - Not called by this routine, but stored in the state of the operation for you to call later if you
 *                    wish.
 */
static afatfsFilePtr_t afatfs_createFileInternal(afatfsFilePtr_t file, const char *name, uint8_t attrib, uint8_t fileMode,
        afatfsFileCallback_t callback)
{
    afatfsCreateFileState_t *opState;

    file->mode = fileMode;

    memset(&file->directoryEntry, 0, sizeof(file->directoryEntry));
    afatfs_findFirst(&file->directoryEntryPos, afatfs.currentDirectoryCluster);

    file->directoryEntry.attrib = attrib;
    fat_convertFilenameToFATStyle(name, (uint8_t*)file->directoryEntry.filename);

    file->operation.operation = AFATFS_FILE_OPERATION_CREATE_FILE;

    opState = &file->operation.state.createFile;

    opState->phase = AFATFS_CREATEFILE_PHASE_INITIAL;
    opState->parentDirectoryCluster = afatfs.currentDirectoryCluster;

    opState->callback = callback;

    afatfs_createFileInternalContinue(file);

    return file;
}

static bool afatfs_fileGetCursorPhysicalPosition(afatfsFilePtr_t file, uint32_t *sector, uint32_t *byteIndexInSector)
{
    if (file->cursorCluster == 0)
        return false; // The cursor is not pointing to a physical location (e.g. pointing at end of file)

    uint32_t sectorIndexInCluster = (file->cursorOffset % afatfs_clusterSize()) / AFATFS_SECTOR_SIZE;

    *sector = afatfs_fileClusterToPhysical(file->cursorCluster, sectorIndexInCluster);
    *byteIndexInSector = file->cursorOffset % AFATFS_SECTOR_SIZE;

    return true;
}

static void afatfs_closeFileContinue(afatfsFilePtr_t file)
{
    afatfsCacheBlockDescriptor_t *descriptor;
    uint32_t sector, byteInSector;

    if (afatfs_saveDirectoryEntry(file) == AFATFS_OPERATION_SUCCESS) {
        // Release our reservation on the directory cache if needed
        if ((file->mode & AFATFS_FILE_MODE_RETAIN_DIRECTORY) != 0) {
            descriptor = afatfs_findCacheSector(afatfs_directorySectorToPhysical(file->directoryEntryPos.clusterNumber, file->directoryEntryPos.sectorNumber));

            if (descriptor) {
                descriptor->retainCount = MAX(descriptor->retainCount - 1, 0);
            }
        }

        // Release our lock on the sector at the cursor
        if (afatfs_fileGetCursorPhysicalPosition(file, &sector, &byteInSector)) {
            descriptor = afatfs_findCacheSector(sector);

            if (descriptor) {
                descriptor->locked = 0;
            }
        }

        file->operation.operation = AFATFS_FILE_OPERATION_NONE;
    }
}

void afatfs_fclose(afatfsFilePtr_t file)
{
    if (file->open) {
        file->open = false;

        file->operation.operation = AFATFS_FILE_OPERATION_CLOSE;
        afatfs_closeFileContinue(file);
    }
}

static void afatfs_mkdirInternal(const char *name, afatfsFileCallback_t callback)
{
    afatfsFilePtr_t file = afatfs_allocateFileHandle();

    if (file) {
        afatfs_createFileInternal(file, name, FAT_FILE_ATTRIBUTE_DIRECTORY, 0, callback);
    }
    // TODO error handling
}

/**
 * Supported file mode strings:
 *
 * r - Read from an existing file TODO
 * w - Create a file for write access, if the file already exists then erase it TODO
 * a - Create a file for write access to the end of the file only, if the file already exists then append to it TODO
 * s - Create a contiguous file for append access only. If the file is already non-empty then a fatal error occurs.
 *     If freefile support is not compiled in then the file is opened in 'a' mode instead.
 *
 * r+ - Read and write from an existing file TODO
 * w+ - Read and write from an existing file, if the file doesn't already exist it is created TODO
 * a+ - Read from or append to an existing file, if the file doesn't already exist it is created TODO
 *
 * All other mode strings are illegal.
 */
bool afatfs_fopen(const char *filename, const char *mode, afatfsFileCallback_t complete)
{
    uint8_t fileMode = 0;
    afatfsFilePtr_t file;

    switch (mode[0]) {
        case 'r':
            fileMode = AFATFS_FILE_MODE_READ;
        break;
        case 'w':
            fileMode = AFATFS_FILE_MODE_WRITE | AFATFS_FILE_MODE_CREATE;
        break;
        case 'a':
            fileMode = AFATFS_FILE_MODE_APPEND | AFATFS_FILE_MODE_CREATE;
        break;
        case 's':
            fileMode = AFATFS_FILE_MODE_APPEND | AFATFS_FILE_MODE_CREATE;
#ifdef AFATFS_USE_FREESPACE_FILE
            fileMode |= AFATFS_FILE_MODE_CONTIGUOUS | AFATFS_FILE_MODE_RETAIN_DIRECTORY;
#endif
        break;
    }

    if (mode[1] == '+') {
        fileMode |= AFATFS_FILE_MODE_READ;

        if (fileMode == AFATFS_FILE_MODE_READ) {
            fileMode |= AFATFS_FILE_MODE_WRITE;
        }
    }

    file = afatfs_allocateFileHandle();

    if (file) {
        afatfs_createFileInternal(file, filename, FAT_FILE_ATTRIBUTE_ARCHIVE, fileMode, complete);
    }

    return file != NULL;
}

/**
 * Attempt to add a free cluster to the end of the given file.
 *
 * Returns true if the operation is complete, otherwise call again later to continue.
 */
static bool afatfs_appendFreeClusterContinue(afatfsFile_t *file)
{
    afatfsAppendFreeClusterState_t *opState = &file->operation.state.appendFreeCluster;
    afatfsOperationStatus_e status;

    doMore:

    switch (opState->phase) {
        case AFATFS_APPEND_FREE_CLUSTER_PHASE_INIT:
            if (opState->previousCluster == 0) {
                // Creating the file from scratch
                opState->searchCluster = FAT_SMALLEST_LEGAL_CLUSTER_NUMBER;
            } else {
                // Begin our search for free clusters at the end of the currently allocated file
                uint32_t fileStartCluster = (file->directoryEntry.firstClusterHigh << 16) | file->directoryEntry.firstClusterLow;
                opState->searchCluster = fileStartCluster + (file->directoryEntry.fileSize + afatfs_clusterSize() - 1) / afatfs_clusterSize();
            }

            opState->phase = AFATFS_APPEND_FREE_CLUSTER_PHASE_FIND_FREESPACE;
            goto doMore;
        break;
        case AFATFS_APPEND_FREE_CLUSTER_PHASE_FIND_FREESPACE:
            status = afatfs_findClusterWithCondition(CLUSTER_SEARCH_FREE_SECTOR, &opState->searchCluster);

            if (status == AFATFS_OPERATION_SUCCESS) {
                // file->directoryEntry.fileSize += afatfs_clusterSize();

                opState->phase = AFATFS_APPEND_FREE_CLUSTER_PHASE_UPDATE_FAT1;
                goto doMore;
            } else if (status == AFATFS_OPERATION_FAILURE) {
                //TODO notify callback of failure
                file->operation.operation = AFATFS_FILE_OPERATION_NONE;
                return false;
            }
        break;
        case AFATFS_APPEND_FREE_CLUSTER_PHASE_UPDATE_FAT1:
            // Terminate the next cluster
            status = afatfs_FATSetNextCluster(opState->searchCluster, 0xFFFFFFFF);

            if (status == AFATFS_OPERATION_SUCCESS) {
                opState->phase = AFATFS_APPEND_FREE_CLUSTER_PHASE_UPDATE_FAT2;
                goto doMore;
            }
        break;
        case AFATFS_APPEND_FREE_CLUSTER_PHASE_UPDATE_FAT2:
            // Add the next cluster to the chain
            status = afatfs_FATSetNextCluster(opState->previousCluster, opState->searchCluster);

            if (status == AFATFS_OPERATION_SUCCESS) {
                opState->phase = AFATFS_APPEND_FREE_CLUSTER_PHASE_UPDATE_FILE_DIRECTORY;
                goto doMore;
            }
        break;
        case AFATFS_APPEND_FREE_CLUSTER_PHASE_UPDATE_FILE_DIRECTORY:
            if (opState->previousCluster == 0) {
                // This is the new first cluster in the file so we need to update the directory entry
                file->directoryEntry.firstClusterHigh = opState->searchCluster >> 16;
                file->directoryEntry.firstClusterLow = opState->searchCluster & 0xFFFF;

                if (afatfs_saveDirectoryEntry(file) == AFATFS_OPERATION_SUCCESS) {
                    opState->phase = AFATFS_APPEND_FREE_CLUSTER_PHASE_COMPLETE;
                    goto doMore;
                }
            } else {
                opState->phase = AFATFS_APPEND_FREE_CLUSTER_PHASE_COMPLETE;
                goto doMore;
            }
        break;
        case AFATFS_APPEND_FREE_CLUSTER_PHASE_COMPLETE:
            file->operation.operation = AFATFS_FILE_OPERATION_NONE;
            return true;
        break;
    }

    return false;
}

/**
 * Attempt to append a free cluster to the end of the file (the current last cluster of the file is `previousCluster`).
 *
 * Returns true if the operation was completed immediately, or false if the operation was queued and will complete
 * later.
 */
bool afatfs_appendFreeCluster(afatfsFilePtr_t file, uint32_t previousCluster)
{
    if (file->operation.operation != AFATFS_FILE_OPERATION_NONE) {
        return false; // File is already busy
    }

    file->operation.operation = AFATFS_FILE_OPERATION_APPEND_FREE_CLUSTER;
    file->operation.state.appendFreeCluster.phase = AFATFS_APPEND_FREE_CLUSTER_PHASE_INIT;
    file->operation.state.appendFreeCluster.previousCluster = previousCluster;

    return afatfs_appendFreeClusterContinue(file);
}

/**
 * Attempt to add a supercluster to the end of the given file.
 *
 * previousCluster - The last cluster of the file to append after, or zero to append to an empty file.
 *
 * Returns 0 if the operation failed, or the first cluster number of the new superblock if the append operation was
 * queued successfully.
 */
static void afatfs_appendSuperclusterContinue(afatfsFile_t *file)
{
    afatfsAppendSuperclusterState_t *opState = &file->operation.state.appendSupercluster;
    afatfsOperationStatus_e status;
    uint32_t freeFileStartCluster;

    uint32_t superClusterSize = afatfs_superClusterSize();

    doMore:

    switch (opState->phase) {
        case AFATFS_APPEND_SUPERCLUSTER_PHASE_INIT:
            // Our file steals the first cluster of the freefile
            freeFileStartCluster = (afatfs.freeFile.directoryEntry.firstClusterHigh << 16) | afatfs.freeFile.directoryEntry.firstClusterLow;

            // The new supercluster needs to be marked with a terminator in the FAT
            opState->fatRewriteStartCluster = freeFileStartCluster;
            opState->fatRewriteEndCluster = freeFileStartCluster + afatfs_fatEntriesPerSector();

            if (opState->previousCluster == 0) {
                // This is the new first cluster in the file so we need to update the directory entry
                file->directoryEntry.firstClusterHigh = afatfs.freeFile.directoryEntry.firstClusterHigh;
                file->directoryEntry.firstClusterLow = afatfs.freeFile.directoryEntry.firstClusterLow;
            } else {
                /*
                 * We also need to update the FAT of the supercluster that used to end the file so that it no longer
                 * terminates there
                 */
                opState->fatRewriteStartCluster -= afatfs_fatEntriesPerSector();
            }

            // Remove the first supercluster from the freefile
            uint32_t newFreeFileStartCluster = freeFileStartCluster + afatfs_fatEntriesPerSector();

            afatfs.freeFile.directoryEntry.firstClusterLow = newFreeFileStartCluster;
            afatfs.freeFile.directoryEntry.firstClusterHigh = newFreeFileStartCluster >> 16;
            afatfs.freeFile.directoryEntry.fileSize -= superClusterSize;

            opState->phase = AFATFS_APPEND_SUPERCLUSTER_PHASE_UPDATE_FAT;
            goto doMore;
        break;
        case AFATFS_APPEND_SUPERCLUSTER_PHASE_UPDATE_FAT:
            status = afatfs_writeContiguousDedicatedFATChain(&opState->fatRewriteStartCluster, opState->fatRewriteEndCluster);

            if (status == AFATFS_OPERATION_SUCCESS) {
                opState->phase = AFATFS_APPEND_SUPERCLUSTER_PHASE_UPDATE_FREEFILE_DIRECTORY;
                goto doMore;
            }
        break;
        case AFATFS_APPEND_SUPERCLUSTER_PHASE_UPDATE_FREEFILE_DIRECTORY:
            if (afatfs_saveDirectoryEntry(&afatfs.freeFile) == AFATFS_OPERATION_SUCCESS) {
                opState->phase = AFATFS_APPEND_SUPERCLUSTER_PHASE_UPDATE_FILE_DIRECTORY;
                goto doMore;
            }
        break;
        case AFATFS_APPEND_SUPERCLUSTER_PHASE_UPDATE_FILE_DIRECTORY:
            if (afatfs_saveDirectoryEntry(file) == AFATFS_OPERATION_SUCCESS) {
                file->operation.operation = AFATFS_FILE_OPERATION_NONE;
            }
        break;
    }
}

uint32_t afatfs_appendSupercluster(afatfsFilePtr_t file, uint32_t previousCluster)
{
    uint32_t superClusterSize = afatfs_superClusterSize();
    uint32_t result;

    if (file->operation.operation != AFATFS_FILE_OPERATION_NONE || afatfs.freeFile.directoryEntry.fileSize < superClusterSize) {
        return 0;
    }

    file->operation.operation = AFATFS_FILE_OPERATION_APPEND_SUPERCLUSTER;
    file->operation.state.appendSupercluster.phase = AFATFS_APPEND_SUPERCLUSTER_PHASE_INIT;
    file->operation.state.appendSupercluster.previousCluster = previousCluster;

    result = (afatfs.freeFile.directoryEntry.firstClusterHigh << 16) | afatfs.freeFile.directoryEntry.firstClusterLow;

    // It's possible for the append to finish immediately, so give that a go now:
    afatfs_appendSuperclusterContinue(file);

    return result;
}

afatfsOperationStatus_e afatfs_fileGetNextCluster(afatfsFilePtr_t file, uint32_t currentCluster, uint32_t *nextCluster)
{
    if ((file->mode & AFATFS_FILE_MODE_CONTIGUOUS) != 0) {
        uint32_t freeFileStart = (afatfs.freeFile.directoryEntry.firstClusterHigh << 16) | afatfs.freeFile.directoryEntry.firstClusterLow;

        // Would the next cluster lie outside the allocated file?
        if (currentCluster + 1 == freeFileStart) {
            *nextCluster = 0;
        } else {
            *nextCluster = currentCluster + 1;
        }

        return AFATFS_OPERATION_SUCCESS;
    } else {
        return afatfs_FATGetNextCluster(0, currentCluster, nextCluster);
    }
}

int afatfs_fwrite(afatfsFilePtr_t file, const uint8_t *buffer, uint32_t len)
{
    if ((file->mode & (AFATFS_FILE_MODE_APPEND | AFATFS_FILE_MODE_WRITE)) == 0) {
        return 0;
    }

    uint32_t clusterSize = afatfs_clusterSize();
    uint32_t cursorOffsetInCluster = file->cursorOffset % clusterSize;

    uint32_t cursorSectorInCluster = cursorOffsetInCluster / AFATFS_SECTOR_SIZE;
    uint32_t cursorOffsetInSector = cursorOffsetInCluster % AFATFS_SECTOR_SIZE;

    uint32_t writtenBytes = 0;

    while (len > 0) {
        // Are we at the end of the file? If so we need to add a cluster
        if (file->cursorCluster == 0) {
            if ((file->mode & AFATFS_FILE_MODE_CONTIGUOUS) != 0) {
                // Steal the first cluster from the beginning of the freefile if we can
                uint32_t newCluster = afatfs_appendSupercluster(file, file->cursorPreviousCluster);

                if (newCluster != 0) {
                    /*
                     * We can go ahead and write to that space before the FAT and directory are updated by the
                     * queued operation:
                     */
                    file->cursorCluster = newCluster;
                } else {
                    /*
                     * Ran out of space on disk or we're already busy trying to extend the file. In the first case we
                     * could opt to change the file mode to regular append and try to continue.
                     */
                    return 0;
                }
            } else if (!afatfs_appendFreeCluster(file, file->cursorPreviousCluster)) {
                // The extension of the file is in progress so please call us again later to try again
                return 0;
            }
        }

        // How many bytes can we write this sector?
        uint32_t bytesToWriteThisSector = AFATFS_SECTOR_SIZE - cursorOffsetInSector;
        bool willSectorBeIncomplete = false;
        uint8_t *sectorBuffer;

        if (len < bytesToWriteThisSector) {
            bytesToWriteThisSector = len;
            willSectorBeIncomplete = true;
        }

        uint8_t cacheFlags = AFATFS_CACHE_WRITE;

        // Lock the sector if it won't be filled, to prevent it being flushed before we fill it up
        if (willSectorBeIncomplete) {
            cacheFlags |= AFATFS_CACHE_LOCK;
        } else {
            // We're done with this sector so it can be flushed to disk
            cacheFlags |= AFATFS_CACHE_UNLOCK;
        }
        /* If there is data before the write point, or there could be data after the write-point
         * then we need to have the original contents of the sector in the cache for us to merge into
         */
        if
            (cursorOffsetInSector > 0
            || (
                willSectorBeIncomplete
                && file->cursorOffset + bytesToWriteThisSector < file->directoryEntry.fileSize
                && (file->mode & AFATFS_FILE_MODE_APPEND) == 0
            )
        ) {
            cacheFlags |= AFATFS_CACHE_READ;
        }

        afatfsOperationStatus_e status = afatfs_cacheSector(
            afatfs_fileClusterToPhysical(file->cursorCluster, cursorSectorInCluster),
            &sectorBuffer,
            cacheFlags
        );

        if (status != AFATFS_OPERATION_SUCCESS) {
            // Not enough cache available to accept this write / sector not ready for read
            return writtenBytes;
        }

        memcpy(sectorBuffer + cursorOffsetInSector, buffer, bytesToWriteThisSector);

        file->cursorOffset += bytesToWriteThisSector;
        file->directoryEntry.fileSize = MAX(file->directoryEntry.fileSize, file->cursorOffset);
        writtenBytes += bytesToWriteThisSector;
        len -= bytesToWriteThisSector;
        buffer += bytesToWriteThisSector;

        if (!willSectorBeIncomplete) {
            cursorSectorInCluster++;
            cursorOffsetInSector = 0;

            if (cursorSectorInCluster == afatfs.sectorsPerCluster) {
                file->cursorPreviousCluster = file->cursorCluster;

                status = afatfs_fileGetNextCluster(file, file->cursorCluster, &file->cursorCluster);

                if (status != AFATFS_OPERATION_SUCCESS) {
                    break;
                }

                cursorSectorInCluster = 0;
            }
        }
    }

    return writtenBytes;
}

static void afatfs_fileOperationContinue(afatfsFile_t *file)
{
    switch (file->operation.operation) {
        case AFATFS_FILE_OPERATION_CREATE_FILE:
            afatfs_createFileInternalContinue(file);
        break;
        case AFATFS_FILE_OPERATION_SEEK:
            afatfs_seekContinue(file);
        break;
        case AFATFS_FILE_OPERATION_CLOSE:
            afatfs_closeFileContinue(file);
        break;
        case AFATFS_FILE_OPERATION_APPEND_SUPERCLUSTER:
            afatfs_appendSuperclusterContinue(file);
        break;
        case AFATFS_FILE_OPERATION_APPEND_FREE_CLUSTER:
            afatfs_appendFreeClusterContinue(file);
        break;
        case AFATFS_FILE_OPERATION_NONE:
            ;
        break;
    }
}

void afatfs_fileOperationsPoll()
{
    afatfs_fileOperationContinue(&afatfs.freeFile);

    for (int i = 0; i < AFATFS_MAX_OPEN_FILES; i++) {
        afatfs_fileOperationContinue(&afatfs.openFiles[i]);
    }
}

#ifdef AFATFS_USE_FREESPACE_FILE

/**
 * Call to set up the initial state for finding the largest block of free space on the device whose corresponding FAT
 * sectors are themselves entirely free space (so the free space has dedicated FAT sectors of its own).
 */
static void afatfs_findLargestContiguousFreeBlockBegin()
{
    // The first FAT sector has two reserved entries, so it isn't eligible for this search. Start at the next FAT sector.
    afatfs.initState.freeSpaceSearch.candidateStart = afatfs_fatEntriesPerSector();
    afatfs.initState.freeSpaceSearch.candidateEnd = afatfs.initState.freeSpaceSearch.candidateStart;
    afatfs.initState.freeSpaceSearch.bestGapStart = 0;
    afatfs.initState.freeSpaceSearch.bestGapLength = 0;
    afatfs.initState.freeSpaceSearch.phase = AFATFS_FREE_SPACE_SEARCH_PHASE_FIND_HOLE;
}

/**
 * Call to continue the search for the largest contiguous block of free space on the device.
 *
 * Returns:
 *     AFATFS_OPERATION_IN_PROGRESS - SD card is busy, call again later to resume
 *     AFATFS_OPERATION_SUCCESS - When the search has finished and afatfs.initState.freeSpaceSearch has been updated with the details of the best gap.
 *     AFATFS_OPERATION_FAILURE - When a read error occured
 */
static afatfsOperationStatus_e afatfs_findLargestContiguousFreeBlockContinue()
{
    afatfsOperationStatus_e result;
    uint32_t fatEntriesPerSector = afatfs_fatEntriesPerSector();

    while (1) {
        switch (afatfs.initState.freeSpaceSearch.phase) {
            case AFATFS_FREE_SPACE_SEARCH_PHASE_FIND_HOLE:
                // Find the first free cluster
                result = afatfs_findClusterWithCondition(CLUSTER_SEARCH_FREE_SECTOR_AT_BEGINNING_OF_FAT_SECTOR, &afatfs.initState.freeSpaceSearch.candidateStart);

                if (result == AFATFS_OPERATION_SUCCESS) {
                    afatfs.initState.freeSpaceSearch.candidateEnd = afatfs.initState.freeSpaceSearch.candidateStart + 1;
                    afatfs.initState.freeSpaceSearch.phase = AFATFS_FREE_SPACE_SEARCH_PHASE_GROW_HOLE;
                } else if (result == AFATFS_OPERATION_FAILURE) {
                    if (afatfs.initState.freeSpaceSearch.candidateStart >= afatfs.numClusters + FAT_SMALLEST_LEGAL_CLUSTER_NUMBER) {
                        // We finished searching the volume (didn't find any more holes to examine)
                        return AFATFS_OPERATION_SUCCESS;
                    } else {
                        // Some sort of read error occured
                        return AFATFS_OPERATION_FAILURE;
                    }
                } else {
                    // In progress, call us again later
                    return result;
                }
            break;
            case AFATFS_FREE_SPACE_SEARCH_PHASE_GROW_HOLE:
                // Find the first used cluster after the beginning of the hole (that signals the end of the hole)
                result = afatfs_findClusterWithCondition(CLUSTER_SEARCH_OCCUPIED_SECTOR, &afatfs.initState.freeSpaceSearch.candidateEnd);

                // Either we found a used sector, or the search reached the end of the volume
                if (result == AFATFS_OPERATION_SUCCESS || afatfs.initState.freeSpaceSearch.candidateEnd >= afatfs.numClusters + FAT_SMALLEST_LEGAL_CLUSTER_NUMBER) {
                    // Constrain the end position to lie at the beginning of a FAT sector (don't include a partial FAT sector as part of the free chain)
                    afatfs.initState.freeSpaceSearch.candidateEnd = MIN(afatfs.initState.freeSpaceSearch.candidateEnd, afatfs.numClusters + FAT_SMALLEST_LEGAL_CLUSTER_NUMBER);

                    uint32_t candidateGapLength = afatfs.initState.freeSpaceSearch.candidateEnd - afatfs.initState.freeSpaceSearch.candidateStart;

                    if (candidateGapLength > afatfs.initState.freeSpaceSearch.bestGapLength) {
                        afatfs.initState.freeSpaceSearch.bestGapStart = afatfs.initState.freeSpaceSearch.candidateStart;
                        afatfs.initState.freeSpaceSearch.bestGapLength = candidateGapLength;
                    }

                    // Start a new seach for a new hole
                    afatfs.initState.freeSpaceSearch.candidateStart = roundUpTo(afatfs.initState.freeSpaceSearch.candidateEnd + 1, fatEntriesPerSector);
                    afatfs.initState.freeSpaceSearch.phase = AFATFS_FREE_SPACE_SEARCH_PHASE_FIND_HOLE;
                } else {
                    return result;
                }
            break;
        }
    }
}

#endif

/**
 * Update the FAT to link the contiguous series of clusters with indexes [*startCluster...endCluster).
 *
 * The FAT sectors for the clusters must not be shared with any other file.
 *
 * Returns -
 *     AFATFS_OPERATION_SUCCESS     - When the entire chain has been written
 *     AFATFS_OPERATION_IN_PROGRESS - Call again later with the updated *startCluster value in order to resume writing.
 */
static afatfsOperationStatus_e afatfs_writeContiguousDedicatedFATChain(uint32_t *startCluster, uint32_t endCluster)
{
    afatfsFATSector_t sector;
    uint32_t fatSectorIndex, fatSectorEntryIndex, fatPhysicalSector;
    uint32_t nextCluster = *startCluster + 1;
    afatfsOperationStatus_e result;

    afatfs_getFATPositionForCluster(*startCluster, &fatSectorIndex, &fatSectorEntryIndex);

    afatfs_assert(fatSectorEntryIndex == 0); // Start cluster must lie on supercluster boundary

    fatPhysicalSector = afatfs_fatSectorToPhysical(0, fatSectorIndex);

    while (*startCluster < endCluster) {
        result = afatfs_cacheSector(fatPhysicalSector, &sector.bytes, AFATFS_CACHE_WRITE | AFATFS_CACHE_DISCARDABLE);

        if (result != AFATFS_OPERATION_SUCCESS)
            return result;

        // Write all the "next cluster" pointers, saving the last cluster to mark as a terminator
        uint32_t entriesToWrite = endCluster - *startCluster - 1;

        if (afatfs.filesystemType == FAT_FILESYSTEM_TYPE_FAT16) {
            entriesToWrite = MIN(entriesToWrite, AFATFS_FAT16_FAT_ENTRIES_PER_SECTOR);

            for (uint32_t i = 0; i < entriesToWrite; i++, nextCluster++) {
                sector.fat16[i] = nextCluster;
            }
        } else {
            entriesToWrite = MIN(entriesToWrite, AFATFS_FAT32_FAT_ENTRIES_PER_SECTOR);

            for (uint32_t i = 0; i < entriesToWrite; i++, nextCluster++) {
                sector.fat32[i] = nextCluster;
            }
        }

        *startCluster += entriesToWrite;

        // Write the terminator for the end of the chain
        if (*startCluster == endCluster - 1) {
            if (afatfs.filesystemType == FAT_FILESYSTEM_TYPE_FAT16) {
                sector.fat16[entriesToWrite] = 0xFFFF;
            } else {
                sector.fat32[entriesToWrite] = 0xFFFFFFFF;
            }

            (*startCluster)++;
            break;
        }

        fatPhysicalSector++;
    }

    return AFATFS_OPERATION_SUCCESS;
}

static void afatfs_freeFileCreated(afatfsFile_t *file)
{
    if (file) {
        // Did the freefile already exist?
        if (file->directoryEntry.fileSize > 0) {
            afatfs.filesystemState = AFATFS_FILESYSTEM_STATE_READY;
        } else {
            // Allocate clusters for the freefile
            afatfs_findLargestContiguousFreeBlockBegin();
            afatfs.substate = AFATFS_SUBSTATE_INITIALIZATION_FREEFILE_FAT_SEARCH;
        }
    } else {
        // Failed to allocate an entry
        afatfs.filesystemState = AFATFS_FILESYSTEM_STATE_FATAL;
    }
}

static void afatfs_initContinue()
{
    uint8_t *sector;
    afatfsOperationStatus_e status;

    doMore:

    switch (afatfs.substate) {
        case AFATFS_SUBSTATE_INITIALIZATION_READ_MBR:
            if (afatfs_cacheSector(0, &sector, AFATFS_CACHE_READ | AFATFS_CACHE_DISCARDABLE) == AFATFS_OPERATION_SUCCESS) {
                if (afatfs_parseMBR(sector)) {
                    afatfs.substate = AFATFS_SUBSTATE_INITIALIZATION_READ_VOLUME_ID;
                    goto doMore;
                } else {
                    afatfs.filesystemState = AFATFS_FILESYSTEM_STATE_FATAL;
                }
            }
        break;
        case AFATFS_SUBSTATE_INITIALIZATION_READ_VOLUME_ID:
            if (afatfs_cacheSector(afatfs.partitionStartSector, &sector, AFATFS_CACHE_READ | AFATFS_CACHE_DISCARDABLE) == AFATFS_OPERATION_SUCCESS) {
                if (afatfs_parseVolumeID(sector)) {
#ifdef AFATFS_USE_FREESPACE_FILE
                    afatfs_createFileInternal(&afatfs.freeFile, AFATFS_FREESPACE_FILENAME, FAT_FILE_ATTRIBUTE_SYSTEM,
                        AFATFS_FILE_MODE_CREATE | AFATFS_FILE_MODE_RETAIN_DIRECTORY, afatfs_freeFileCreated);
                    afatfs.substate = AFATFS_SUBSTATE_INITIALIZATION_FREEFILE_CREATING;
#else
                    afatfs.filesystemState = AFATFS_FILESYSTEM_STATE_READY;
#endif
                } else {
                    afatfs.filesystemState = AFATFS_FILESYSTEM_STATE_FATAL;
                }
            }
        break;
#ifdef AFATFS_USE_FREESPACE_FILE
        case AFATFS_SUBSTATE_INITIALIZATION_FREEFILE_CREATING:
            afatfs_fileOperationsPoll();
        break;
        case AFATFS_SUBSTATE_INITIALIZATION_FREEFILE_FAT_SEARCH:
            if (afatfs_findLargestContiguousFreeBlockContinue() == AFATFS_OPERATION_SUCCESS) {
                uint32_t startCluster = afatfs.initState.freeSpaceSearch.bestGapStart;
                uint32_t endCluster = afatfs.initState.freeSpaceSearch.bestGapStart + MAX(afatfs.initState.freeSpaceSearch.bestGapLength - AFATFS_FREEFILE_LEAVE_CLUSTERS, 0);

                afatfs.initState.freeSpaceFAT.startCluster = startCluster;
                afatfs.initState.freeSpaceFAT.endCluster = endCluster;

                afatfs.freeFile.directoryEntry.firstClusterHigh = startCluster >> 16;
                afatfs.freeFile.directoryEntry.firstClusterLow = startCluster & 0xFFFF;

                /* If the final part of the file doesn't fill an entire FAT sector, trim that part off: */
                afatfs.freeFile.directoryEntry.fileSize = (endCluster - startCluster) / afatfs_fatEntriesPerSector() * afatfs_superClusterSize();

                afatfs.substate = AFATFS_SUBSTATE_INITIALIZATION_FREEFILE_UPDATE_FAT;
                goto doMore;
            }
        break;
        case AFATFS_SUBSTATE_INITIALIZATION_FREEFILE_UPDATE_FAT:
            status = afatfs_writeContiguousDedicatedFATChain(&afatfs.initState.freeSpaceFAT.startCluster, afatfs.initState.freeSpaceFAT.endCluster);

            if (status == AFATFS_OPERATION_SUCCESS) {
                afatfs.substate = AFATFS_SUBSTATE_INITIALIZATION_FREEFILE_SAVE_DIR_ENTRY;

                goto doMore;
            } else if (status == AFATFS_OPERATION_FAILURE) {
                afatfs.filesystemState = AFATFS_FILESYSTEM_STATE_FATAL;
            }
        break;
        case AFATFS_SUBSTATE_INITIALIZATION_FREEFILE_SAVE_DIR_ENTRY:
            status = afatfs_saveDirectoryEntry(&afatfs.freeFile);

            if (status == AFATFS_OPERATION_SUCCESS) {
                afatfs.filesystemState = AFATFS_FILESYSTEM_STATE_READY;
            } else if (status == AFATFS_OPERATION_FAILURE) {
                afatfs.filesystemState = AFATFS_FILESYSTEM_STATE_FATAL;
            }
        break;
#endif
    }
}

void afatfs_poll()
{
    sdcard_poll();

    afatfs_flush();

    switch (afatfs.filesystemState) {
        case AFATFS_FILESYSTEM_STATE_INITIALIZATION:
            afatfs_initContinue();
        break;
        case AFATFS_FILESYSTEM_STATE_READY:
            afatfs_fileOperationsPoll();
        break;
        default:
            ;
    }
}

afatfsFilesystemState_e afatfs_getFilesystemState()
{
    return afatfs.filesystemState;
}

void afatfs_init()
{
    afatfs.filesystemState = AFATFS_FILESYSTEM_STATE_INITIALIZATION;
    afatfs.substate = AFATFS_SUBSTATE_INITIALIZATION_READ_MBR;

    afatfs_poll();
}

/**
 * Shut down the filesystem, flushing all data to the disk. Keep calling until it returns true.
 */
bool afatfs_destroy()
{
#ifdef AFATFS_USE_FREESPACE_FILE
    if (afatfs.freeFile.open) {
        afatfs_fclose(&afatfs.freeFile);
    }
#endif

    //TODO close open files
    afatfs_poll();

    for (int i = 0; i < AFATFS_NUM_CACHE_SECTORS; i++) {
        // Flush even if the pages are "locked"
        if (afatfs.cacheDescriptor[i].state == AFATFS_CACHE_STATE_DIRTY) {
            if (sdcard_writeBlock(afatfs.cacheDescriptor[i].sectorIndex, afatfs_cacheSectorGetMemory(i), afatfs_sdcardWriteComplete, 0)) {
                afatfs.cacheDescriptor[i].state = AFATFS_CACHE_STATE_WRITING;
            }
            return false;
        } else if (afatfs.cacheDescriptor[i].state == AFATFS_CACHE_STATE_WRITING) {
            return false;
        }
    }

    afatfs.filesystemState = AFATFS_FILESYSTEM_STATE_UNKNOWN;

    return true;

}

uint32_t afatfs_getContiguousFreeSpace()
{
    return afatfs.freeFile.directoryEntry.fileSize;
}
