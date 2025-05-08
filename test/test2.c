#include <stdio.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <unistd.h>
//#include <linux/ioctl.h>

#define DEVICE_SIZE (8 * 4096)
#define NUM_REPEAT (3)

#define DYNMMAPDEV_IOC_MAGIC 'd'
#define DYNMMAPDEV_IOC_UNMAP_PAGE _IOW(DYNMMAPDEV_IOC_MAGIC, 1, unsigned long)
struct ioctl_data {
    unsigned long addr;
    unsigned long offset;
};

int main()
{
    int fd = open("/dev/dynmmapdev", O_RDWR);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    unsigned char *map = mmap(NULL, DEVICE_SIZE * 2, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        perror("mmap");
        close(fd);
        return 1;
    }

    for (int repeat_cnt = 0; repeat_cnt < NUM_REPEAT; repeat_cnt++) {
        for (int i = 0; i < DEVICE_SIZE; i += 4096) {
            printf("Ptr %p = Offset 0x%x: value=0x%02x\n", &map[i], i, map[i]);
            struct ioctl_data data = {.addr=(unsigned long)&map[i], .offset=DEVICE_SIZE};
            ioctl(fd, DYNMMAPDEV_IOC_UNMAP_PAGE, &data);
        }
    }

    munmap(map, DEVICE_SIZE * 2);
    close(fd);

    return 0;
}
