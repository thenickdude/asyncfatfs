DEBUG_FLAGS	 = -g3 -ggdb -DAFATFS_DEBUG

CFLAGS = -O0 \
	$(DEBUG_FLAGS) \
	-std=gnu99 \
	-Wall -pedantic -Wextra -Wshadow \
	-Ilib/

AFATFS_SOURCE = lib/fat_standard.c lib/asyncfatfs.c
TEST_SOURCE = tests/sdcard_sim.c tests/common.c
SDCARD_TEMP_FILE = tests/sdcard_temp.dmg

.PHONY: all test test-long clean test-binaries

all: test-binaries

test-binaries: tests/test_root_fill tests/test_subdir_fill tests/test_volume_fill tests/test_file_modes tests/test_file_delete tests/test_logging_workload

test-long : test
	@echo ""
	@echo "Testing with 16GB FAT32 volume"
	@echo ""
	
	@gunzip --stdout images/blank_fat32_16gb.dmg.gz > $(SDCARD_TEMP_FILE)
	@tests/test_root_fill $(SDCARD_TEMP_FILE)
	
	@gunzip --stdout images/blank_fat32_16gb.dmg.gz > $(SDCARD_TEMP_FILE)
	@tests/test_subdir_fill $(SDCARD_TEMP_FILE)
	
	@gunzip --stdout images/blank_fat32_16gb.dmg.gz > $(SDCARD_TEMP_FILE)
	@tests/test_volume_fill $(SDCARD_TEMP_FILE)
	
	@rm $(SDCARD_TEMP_FILE)

test : test-binaries
	@echo ""
	@echo "Testing with 100MB FAT16 volume"
	@echo ""
	
	@gunzip --stdout images/blank_fat16_100mb.dmg.gz > $(SDCARD_TEMP_FILE)
	@tests/test_root_fill $(SDCARD_TEMP_FILE)
	
	@gunzip --stdout images/blank_fat16_100mb.dmg.gz > $(SDCARD_TEMP_FILE)
	@tests/test_subdir_fill $(SDCARD_TEMP_FILE)
	
	@gunzip --stdout images/blank_fat16_100mb.dmg.gz > $(SDCARD_TEMP_FILE)
	@tests/test_volume_fill $(SDCARD_TEMP_FILE)
	
	@gunzip --stdout images/blank_fat16_100mb.dmg.gz > $(SDCARD_TEMP_FILE)
	@tests/test_logging_workload $(SDCARD_TEMP_FILE)
	
	@gunzip --stdout images/blank_fat16_100mb.dmg.gz > $(SDCARD_TEMP_FILE)
	@tests/test_file_modes $(SDCARD_TEMP_FILE)
	
	@gunzip --stdout images/blank_fat16_100mb.dmg.gz > $(SDCARD_TEMP_FILE)
	@tests/test_file_delete $(SDCARD_TEMP_FILE)
	
	@echo ""
	@echo "Testing with 2GB FAT16 volume"
	@echo ""
	
	@gunzip --stdout images/blank_fat16_2gb.dmg.gz > $(SDCARD_TEMP_FILE)
	@tests/test_root_fill $(SDCARD_TEMP_FILE)
	
	@gunzip --stdout images/blank_fat16_2gb.dmg.gz > $(SDCARD_TEMP_FILE)
	@tests/test_subdir_fill $(SDCARD_TEMP_FILE)
	
	@gunzip --stdout images/blank_fat16_2gb.dmg.gz > $(SDCARD_TEMP_FILE)
	@tests/test_volume_fill $(SDCARD_TEMP_FILE)
	
	@gunzip --stdout images/blank_fat16_100mb.dmg.gz > $(SDCARD_TEMP_FILE)
	@tests/test_logging_workload $(SDCARD_TEMP_FILE)
	
	@echo ""
	@echo "Testing with 2.5GB FAT32 volume"
	@echo ""
	
	@gunzip --stdout images/blank_fat32_2.5gb.dmg.gz > $(SDCARD_TEMP_FILE)
	@tests/test_root_fill $(SDCARD_TEMP_FILE)
	
	@gunzip --stdout images/blank_fat32_2.5gb.dmg.gz > $(SDCARD_TEMP_FILE)
	@tests/test_subdir_fill $(SDCARD_TEMP_FILE)
	
	@gunzip --stdout images/blank_fat32_2.5gb.dmg.gz > $(SDCARD_TEMP_FILE)
	@tests/test_volume_fill $(SDCARD_TEMP_FILE)
	
	@gunzip --stdout images/blank_fat16_100mb.dmg.gz > $(SDCARD_TEMP_FILE)
	@tests/test_logging_workload $(SDCARD_TEMP_FILE)
	
	@rm $(SDCARD_TEMP_FILE)

tests/test_root_fill : $(AFATFS_SOURCE) $(TEST_SOURCE) tests/test_root_fill.c
tests/test_subdir_fill : $(AFATFS_SOURCE) $(TEST_SOURCE) tests/test_subdir_fill.c
tests/test_volume_fill : $(AFATFS_SOURCE) $(TEST_SOURCE) tests/test_volume_fill.c
tests/test_file_modes : $(AFATFS_SOURCE) $(TEST_SOURCE) tests/test_file_modes.c
tests/test_file_delete : $(AFATFS_SOURCE) $(TEST_SOURCE) tests/test_file_delete.c
tests/test_logging_workload : $(AFATFS_SOURCE) $(TEST_SOURCE) tests/test_logging_workload.c

clean :
	rm -f tests/test_root_fill tests/test_subdir_fill tests/test_volume_fill tests/test_file_modes tests/test_file_delete