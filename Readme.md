## AsyncFatFS

When paired with a simple asynchronous read-block/write-block API for an SD card, this library provides an 
asynchronous FAT16/FAT32 filesystem for embedded devices. Filesystem operations do not wait for the SD card
to become ready, but instead either provide a callback to notify you of operation completion, or return a
status code that indicates that the operation should be retried later.

A special feature of this filesystem is a high-speed contiguous append file mode, provided by the "freefile"
support. In this mode, the largest contiguous free block on the volume is pre-allocated during filesystem 
initialisation into a file called "freespac.e". One file can be created which slowly grows from the beginning 
of this contiguous region, stealing the first part of the freefile's space. Because the freefile is contiguous, 
the FAT entries for the file need never be read. This saves on buffer space and reduces latency during file
extend operations.