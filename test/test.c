#include <stdio.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#define DEVICE_SIZE (16 * 4096)

int main()
{
    int fd = open("/dev/dynmmapdev", O_RDWR);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    unsigned char *map = mmap(NULL, DEVICE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        perror("mmap");
        close(fd);
        return 1;
    }

    for (int i = 0; i < DEVICE_SIZE; i += 4096) {
        printf("Ptr %p = Offset 0x%x: value=0x%02x\n", &map[i], i, map[i]);
    }

    munmap(map, DEVICE_SIZE);
    close(fd);

    return 0;
}
