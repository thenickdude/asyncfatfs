#include <stdint.h>
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
#define AFATFS_SUBSTATE_INITIALIZATION_FREEFILE_FIND             2
#define AFATFS_SUBSTATE_INITIALIZATION_FREEFILE_CREATE_DIR_ENTRY 3
#define AFATFS_SUBSTATE_INITIALIZATION_FREEFILE_FAT_SEARCH           4
#define AFATFS_SUBSTATE_INITIALIZATION_FREEFILE_UPDATE_FAT       5
#define AFATFS_SUBSTATE_INITIALIZATION_FREEFILE_SAVE_DIR_ENTRY   6

// Turn the largest free block on the disk into one contiguous file for efficient fragment-free allocation
#define AFATFS_USE_FREESPACE_FILE

#define AFATFS_FREESPACE_FILENAME "FREESPACE  "

#define MIN(a, b) ((a) < (b) ? (a) : (b))

typedef enum {
    AFATFS_CACHE_STATE_EMPTY,
    AFATFS_CACHE_STATE_READING,
    AFATFS_CACHE_STATE_IN_SYNC,
    AFATFS_CACHE_STATE_DIRTY,
    AFATFS_CACHE_STATE_WRITING
} afatfsCacheBlockState_e;

typedef union afatfsFATSector_t {
    uint8_t *bytes;
    uint16_t *fat16;
    uint32_t *fat32;
} afatfsFATSector_t;

typedef struct afatfsFile_t {
    uint32_t filePtrOffset;
    uint32_t numClusters;

    afatfsDirEntryPointer_t directoryEntry;
} afatfsFile_t;

typedef struct afatfsCacheBlockDescriptor_t {
    uint32_t sectorIndex;
    afatfsCacheBlockState_e state;
    uint32_t lastUse;

    /*
     * The state of this block must not transition (e.g. do not flush to disk). This is useful for a sector which
     * is currently being written to by the application (so flushing it would be a waste of time).
     */
    unsigned locked:1;

    /*
     * If this block is in the In Sync state, it should be discarded from the cache in preference to other blocks.
     * This is useful for data that we don't expect to read again, e.g. data written to an append-only file.
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
    afatfsDirEntryPointer_t freeSpaceFile;
#endif

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

static uint32_t roundUpTo(uint32_t value, uint32_t rounding)
{
    uint32_t remainder = value % rounding;

    if (remainder) {
        value += rounding - remainder;
    }

    return value;
}

static bool afatfs_assert(bool condition)
{
    if (!condition) {
        afatfs.filesystemState = AFATFS_FILESYSTEM_STATE_FATAL;
    }

    return condition;
}

static uint32_t afatfs_fatEntriesPerSector()
{
    return afatfs.filesystemType == FAT_FILESYSTEM_TYPE_FAT32 ? AFATFS_FAT32_FAT_ENTRIES_PER_SECTOR : AFATFS_FAT16_FAT_ENTRIES_PER_SECTOR;
}

static uint8_t *afatfs_getCacheSectorMemory(int cacheSectorIndex)
{
    return afatfs.cache + cacheSectorIndex * AFATFS_SECTOR_SIZE;
}

static void afatfs_initCacheSector(afatfsCacheBlockDescriptor_t *descriptor, uint32_t sectorIndex, bool locked)
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
            afatfs_assert(afatfs_getCacheSectorMemory(i) == buffer && afatfs.cacheDescriptor[i].state == AFATFS_CACHE_STATE_READING);

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
            afatfs_assert(afatfs_getCacheSectorMemory(i) == buffer);

            afatfs.cacheDescriptor[i].state = AFATFS_CACHE_STATE_IN_SYNC;
            break;
        }
    }
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
                if (!afatfs.cacheDescriptor[i].locked) {
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
        afatfs_initCacheSector(&afatfs.cacheDescriptor[allocateIndex], sectorIndex, false);
    }

    return allocateIndex;
}

/**
 * Attempt to flush dirty cache pages out to the card, returning true if all data has been flushed.
 */
bool afatfs_flush()
{
    if (afatfs.cacheDirtyEntries > 0) {
        for (int i = 0; i < AFATFS_NUM_CACHE_SECTORS; i++) {
            if (afatfs.cacheDescriptor[i].state == AFATFS_CACHE_STATE_DIRTY && !afatfs.cacheDescriptor[i].locked) {
                if (sdcard_writeBlock(afatfs.cacheDescriptor[i].sectorIndex, afatfs_getCacheSectorMemory(i), afatfs_sdcardWriteComplete, 0)) {
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

static uint32_t afatfs_clusterToPhysical(uint32_t clusterNumber, uint32_t sectorIndex)
{
    return afatfs.clusterStartSector + (clusterNumber - 2) * afatfs.sectorsPerCluster + sectorIndex;
}

static afatfsOperationStatus_e afatfs_getSectorForRead(uint32_t physicalSectorIndex, uint8_t **buffer, bool markDirty)
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
            if (sdcard_readBlock(physicalSectorIndex, afatfs_getCacheSectorMemory(cacheSectorIndex), afatfs_sdcardReadComplete, 0)) {
                afatfs.cacheDescriptor[cacheSectorIndex].state = AFATFS_CACHE_STATE_READING;
            }
            return AFATFS_OPERATION_IN_PROGRESS;
        break;

        case AFATFS_CACHE_STATE_WRITING:
        case AFATFS_CACHE_STATE_IN_SYNC:
            if (markDirty) {
                afatfs.cacheDescriptor[cacheSectorIndex].state = AFATFS_CACHE_STATE_DIRTY;
                afatfs.cacheDirtyEntries++;
            }
            // Fall through

        case AFATFS_CACHE_STATE_DIRTY:
            *buffer = afatfs_getCacheSectorMemory(cacheSectorIndex);

            return AFATFS_OPERATION_SUCCESS;
        break;

        default:
            return AFATFS_OPERATION_FAILURE;
    }
}

/**
 * Get a cache entry for the given sector that is suitable for write only (no read!)
 *
 * lock        - True if the sector should not be flushed to disk yet.
 * discardable - Set to true as a hint that this sector needn't be retained in cache after writing.
 */
static afatfsOperationStatus_e afatfs_getSectorForWrite(uint32_t physicalSectorIndex, uint8_t **buffer, bool lock, bool discardable)
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
            afatfs.cacheDescriptor[cacheSectorIndex].discardable = discardable ? 1 : 0;
            // Fall through

        case AFATFS_CACHE_STATE_WRITING:
        case AFATFS_CACHE_STATE_IN_SYNC:
            afatfs.cacheDescriptor[cacheSectorIndex].state = AFATFS_CACHE_STATE_DIRTY;
            afatfs.cacheDirtyEntries++;
            // Fall through

        case AFATFS_CACHE_STATE_DIRTY:
            afatfs.cacheDescriptor[cacheSectorIndex].locked = (afatfs.cacheDescriptor[cacheSectorIndex].locked || lock) ? 1 : 0;

            *buffer = afatfs_getCacheSectorMemory(cacheSectorIndex);

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

static bool isPowerOfTwo(unsigned int x)
{
    return ((x != 0) && ((x & (~x + 1)) == x));
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

    afatfs.clusterStartSector = endOfFATs + afatfs.rootDirectorySectors;

    return true;
}

/**
 * Get the position of the entry in the FAT for the cluster with the given number.
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
static afatfsOperationStatus_e afatfs_getNextCluster(int fatIndex, uint32_t cluster, uint32_t *nextCluster)
{
    afatfsFATSector_t sector;

    uint32_t fatSectorIndex, fatSectorEntryIndex;

    afatfs_getFATPositionForCluster(cluster, &fatSectorIndex, &fatSectorEntryIndex);

    afatfsOperationStatus_e result =  afatfs_getSectorForRead(afatfs_fatSectorToPhysical(fatIndex, fatSectorIndex), &sector.bytes, false);

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
 * Convert the given "prefix.ext" style filename to the FAT format to be stored on disk.
 *
 * fatFilename must point to a buffer which is 11 bytes long. The buffer is not null-terminated.
 */
static void afatfs_convertFilenameToFATStyle(const char *filename, char *fatFilename)
{
    for (int i = 0; i < 8; i++) {
        if (*filename == '\0' || *filename == '.') {
            *fatFilename = ' ';
        } else {
            *fatFilename = *filename;
            filename++;
        }
        fatFilename++;
    }

    if (*filename == '.') {
        filename++;
    }

    for (int i = 0; i < 3; i++) {
         if (*filename == '\0') {
             *fatFilename = ' ';
         } else {
             *fatFilename = *filename;
             filename++;
         }
         fatFilename++;
     }
}

static uint32_t afatfs_directorySectorToPhysical(uint32_t clusterNumber, uint32_t sectorNumber)
{
    if (clusterNumber == 0) {
        // FAT16 root directory
        return afatfs.fatStartSector + AFATFS_NUM_FATS * afatfs.fatSectors + sectorNumber;
    } else {
        return afatfs_clusterToPhysical(clusterNumber, sectorNumber);
    }
}

/**
 * Attempt to advance the directory pointer to the next entry in the directory.
 *
 * Returns AFATFS_OPERATION_SUCCESS on success and loads the next entry's details into the finder.
 * Returns AFATFS_OPERATION_IN_PROGRESS when the disk is busy. The pointer is not advanced, call again later to retry.
 */
static afatfsOperationStatus_e afatfs_findNext(afatfsDirEntryPointer_t *finder)
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

            result = afatfs_getNextCluster(0, finder->clusterNumber, &nextCluster);

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

    result = afatfs_getSectorForRead(afatfs_directorySectorToPhysical(finder->clusterNumber, finder->sectorNumber), &sector, false);

    if (result == AFATFS_OPERATION_SUCCESS) {
        finder->entryIndex++;
        finder->finished = false;
        memcpy(&finder->entry, sector + finder->entryIndex * sizeof(fatDirectoryEntry_t), sizeof(fatDirectoryEntry_t));
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
 * Allocate a new directory entry for a new file with the given FAT filename (11 characters, use
 * afatfs_convertFilenameToFATString()).
 *
 * Before the first call to this function, call afatfs_findFirst() on the dirEntry to initialise it for the directory
 * you want to create the file inside.
 *
 * The allocated entry is not written to the sector. To write the entry to disk, call afatfs_saveDirectoryEntry().
 */
static afatfsOperationStatus_e afatfs_createDirectoryEntry(afatfsDirEntryPointer_t *dirEntry, const char *filename)
{
    afatfsOperationStatus_e result;

    while ((result = afatfs_findNext(dirEntry)) == AFATFS_OPERATION_SUCCESS) {
        if (dirEntry->finished) {
            // We got to the end of the directory chain without finding a free space to store our file.
            // We need to extend the chain

            // TODO
            afatfs.filesystemState = AFATFS_FILESYSTEM_STATE_FATAL;
            return AFATFS_OPERATION_FAILURE;
        } else if (fat_isDirectoryEntryEmpty(&dirEntry->entry) || fat_isDirectoryEntryTerminator(&dirEntry->entry)) {
            memset(&dirEntry->entry, 0, sizeof(dirEntry->entry));

            dirEntry->entry.attrib = FAT_FILE_ATTRIBUTE_ARCHIVE;
            memcpy(dirEntry->entry.filename, filename, 11);

            return AFATFS_OPERATION_SUCCESS;
        }
    }

    return result;
}

/**
 * Write the given directory entry to its corresponding directory sector.
 */
static afatfsOperationStatus_e afatfs_saveDirectoryEntry(afatfsDirEntryPointer_t *dirEntry)
{
    uint8_t *sector;
    afatfsOperationStatus_e result;

    result = afatfs_getSectorForRead(afatfs_directorySectorToPhysical(dirEntry->clusterNumber, dirEntry->sectorNumber), &sector, true);

    if (result == AFATFS_OPERATION_SUCCESS) {
        memcpy(sector + dirEntry->entryIndex * FAT_DIRECTORY_ENTRY_SIZE, &dirEntry->entry, FAT_DIRECTORY_ENTRY_SIZE);
    }

    return result;
}

typedef enum {
    CLUSTER_SEARCH_FREE_SECTOR_AT_BEGINNING_OF_FAT_SECTOR,
    CLUSTER_SEARCH_OCCUPIED_SECTOR,
} afatfsClusterSearchCondition_e;

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
    bool lookingForFree = condition == CLUSTER_SEARCH_FREE_SECTOR_AT_BEGINNING_OF_FAT_SECTOR;

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
            jump = 1;
        break;
    }

    while (*cluster < afatfs.numClusters + FAT_SMALLEST_LEGAL_CLUSTER_NUMBER) {
        afatfsOperationStatus_e result = afatfs_getSectorForRead(afatfs_fatSectorToPhysical(0, fatSectorIndex), &sector.bytes, false);

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

#ifdef AFATFS_USE_FREESPACE_FILE

/**
 * Call to set up the initial state for finding the largest block of free space on the device whose corresponding FAT
 * sectors are themselves entirely free space (so the free space has dedicated FAT sectors of its own).
 */
static void afatfs_beginFindLargestContiguousFreeBlock()
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
static afatfsOperationStatus_e afatfs_continueFindLargestContiguousFreeBlock()
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
    afatfs_assert(fatSectorEntryIndex == 0);

    fatPhysicalSector = afatfs_fatSectorToPhysical(0, fatSectorIndex);

    while (*startCluster < endCluster) {
        result = afatfs_getSectorForWrite(fatPhysicalSector, &sector.bytes, false, true);

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

static void afatfs_continueInit()
{
    uint8_t *sector;
    afatfsOperationStatus_e status;

    doMore:

    switch (afatfs.substate) {
        case AFATFS_SUBSTATE_INITIALIZATION_READ_MBR:
            if (afatfs_getSectorForRead(0, &sector, false) == AFATFS_OPERATION_SUCCESS) {
                if (afatfs_parseMBR(sector)) {
                    afatfs.substate = AFATFS_SUBSTATE_INITIALIZATION_READ_VOLUME_ID;
                    goto doMore;
                } else {
                    afatfs.filesystemState = AFATFS_FILESYSTEM_STATE_FATAL;
                }
            }
        break;
        case AFATFS_SUBSTATE_INITIALIZATION_READ_VOLUME_ID:
            if (afatfs_getSectorForRead(afatfs.partitionStartSector, &sector, false) == AFATFS_OPERATION_SUCCESS) {
                if (afatfs_parseVolumeID(sector)) {
#ifdef AFATFS_USE_FREESPACE_FILE
                    afatfs_findFirst(&afatfs.freeSpaceFile, afatfs.rootDirectoryCluster);

                    afatfs.substate = AFATFS_SUBSTATE_INITIALIZATION_FREEFILE_FIND;
#else
                    afatfs.filesystemState = AFATFS_FILESYSTEM_STATE_READY;
#endif
                } else {
                    afatfs.filesystemState = AFATFS_FILESYSTEM_STATE_FATAL;
                }
            }
        break;
#ifdef AFATFS_USE_FREESPACE_FILE
        case AFATFS_SUBSTATE_INITIALIZATION_FREEFILE_FIND:
            while (afatfs_findNext(&afatfs.freeSpaceFile) == AFATFS_OPERATION_SUCCESS) {
                if (afatfs.freeSpaceFile.finished || fat_isDirectoryEntryTerminator(&afatfs.freeSpaceFile.entry)) {
                    // We couldn't find a freespace file, so create it within the root directory
                    afatfs_findFirst(&afatfs.freeSpaceFile, 0);
                    afatfs.substate = AFATFS_SUBSTATE_INITIALIZATION_FREEFILE_CREATE_DIR_ENTRY;

                    goto doMore;
                } else if (strcmp(afatfs.freeSpaceFile.entry.filename, AFATFS_FREESPACE_FILENAME) == 0) {
                    afatfs.filesystemState = AFATFS_FILESYSTEM_STATE_READY;
                }
            }
        break;
        case AFATFS_SUBSTATE_INITIALIZATION_FREEFILE_CREATE_DIR_ENTRY:
            if (afatfs_createDirectoryEntry(&afatfs.freeSpaceFile, AFATFS_FREESPACE_FILENAME) == AFATFS_OPERATION_SUCCESS) {
                afatfs.freeSpaceFile.entry.attrib = FAT_FILE_ATTRIBUTE_SYSTEM;

                afatfs_beginFindLargestContiguousFreeBlock();
                afatfs.substate = AFATFS_SUBSTATE_INITIALIZATION_FREEFILE_FAT_SEARCH;

                goto doMore;
            }
        break;
        case AFATFS_SUBSTATE_INITIALIZATION_FREEFILE_FAT_SEARCH:
            if (afatfs_continueFindLargestContiguousFreeBlock() == AFATFS_OPERATION_SUCCESS) {
                uint32_t startCluster = afatfs.initState.freeSpaceSearch.bestGapStart;
                uint32_t endCluster = afatfs.initState.freeSpaceSearch.bestGapStart + afatfs.initState.freeSpaceSearch.bestGapLength;

                afatfs.substate = AFATFS_SUBSTATE_INITIALIZATION_FREEFILE_UPDATE_FAT;

                afatfs.initState.freeSpaceFAT.startCluster = startCluster;
                afatfs.initState.freeSpaceFAT.endCluster = endCluster;

                afatfs.freeSpaceFile.entry.firstClusterHigh = startCluster >> 16;
                afatfs.freeSpaceFile.entry.firstClusterLow = startCluster & 0xFFFF;

                /* If the final part of the file doesn't fill an entire FAT sector, trim that part off: */
                afatfs.freeSpaceFile.entry.fileSize =
                        (endCluster - startCluster) / afatfs_fatEntriesPerSector() * afatfs_fatEntriesPerSector()
                            * afatfs.sectorsPerCluster * AFATFS_SECTOR_SIZE;

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
            status = afatfs_saveDirectoryEntry(&afatfs.freeSpaceFile);

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
            afatfs_continueInit();
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

void afatfs_destroy()
{
    afatfs.filesystemState = AFATFS_FILESYSTEM_STATE_UNKNOWN;

    //TODO clear buffers
}
