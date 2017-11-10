
#include <stdio.h>
#include <syslog.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <sys/mman.h>
#include <sys/ioctl.h>

#include <bcm_host.h>

int process() {
    DISPMANX_DISPLAY_HANDLE_T display;
    DISPMANX_MODEINFO_T display_info;
    DISPMANX_RESOURCE_HANDLE_T screen_resource;
    VC_IMAGE_TRANSFORM_T transform;
    uint32_t image_prt;
    VC_RECT_T rect1;
    int ret;
    int fbfd = 0;
    char *fbp = 0;

    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;

    uint16_t *screenbuf[3];
    uint8_t bufNum = 0;

    bcm_host_init();

    display = vc_dispmanx_display_open(0);
    if (!display) {
        syslog(LOG_ERR, "Unable to open primary display");
        return -1;
    }
    ret = vc_dispmanx_display_get_info(display, &display_info);
    if (ret) {
        syslog(LOG_ERR, "Unable to get primary display information");
        return -1;
    }
    syslog(LOG_INFO, "Primary display is %d x %d", display_info.width, display_info.height);


    fbfd = open("/dev/fb1", O_RDWR);
    if (fbfd == -1) {
        syslog(LOG_ERR, "Unable to open secondary display");
        return -1;
    }
    if (ioctl(fbfd, FBIOGET_FSCREENINFO, &finfo)) {
        syslog(LOG_ERR, "Unable to get secondary display information");
        return -1;
    }
    if (ioctl(fbfd, FBIOGET_VSCREENINFO, &vinfo)) {
        syslog(LOG_ERR, "Unable to get secondary display information");
        return -1;
    }

    syslog(LOG_INFO, "Second display is %d x %d %dbps\n", vinfo.xres, vinfo.yres, vinfo.bits_per_pixel);

    screen_resource = vc_dispmanx_resource_create(VC_IMAGE_RGB565, vinfo.xres, vinfo.yres, &image_prt);
    if (!screen_resource) {
        syslog(LOG_ERR, "Unable to create screen buffer");
        close(fbfd);
        vc_dispmanx_display_close(display);
        return -1;
    }

    // Allocate 3 screen buffers (current, prior, delta)
    screenbuf[0] = (uint16_t *)malloc(vinfo.xres * vinfo.yres * 2 * 3);
    if(!screenbuf[0]) {
        printf("malloc fail");
        return -1;
    }
    memset(screenbuf[0], 0, vinfo.xres * vinfo.yres * 2 * 2);
    screenbuf[1] = &screenbuf[0][vinfo.xres * vinfo.yres];
    screenbuf[2] = &screenbuf[1][vinfo.xres * vinfo.yres];

    fbp = (char*) mmap(0, finfo.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fbfd, 0);
    if (fbp <= 0) {
        syslog(LOG_ERR, "Unable to create mamory mapping");
        close(fbfd);
        ret = vc_dispmanx_resource_delete(screen_resource);
        vc_dispmanx_display_close(display);
        return -1;
    }

    vc_dispmanx_rect_set(&rect1, 0, 0, vinfo.xres, vinfo.yres);

    while (1) {
        ret = vc_dispmanx_snapshot(display, screen_resource, 0);
        // Transfer snamshot to screenbuf
        vc_dispmanx_resource_read_data(screen_resource, &rect1,
          (char *)screenbuf[bufNum], vinfo.xres * vinfo.bits_per_pixel / 8);

        // Calculate XOR between two screenbufs
        uint32_t *p0 = (uint32_t *)screenbuf[0], // 32-bit ops
                 *p1 = (uint32_t *)screenbuf[1],
                 *p2 = (uint32_t *)screenbuf[2];
        for(int i=0; i<vinfo.xres * vinfo.yres / 2; i++) {
          p2[i] = p0[i] ^ p1[i];
        }

        // Find bounding rect of nonzero pixels
        uint32_t first, last;

        // Find first nonzero lword
        for(first=0; (first < (vinfo.xres * vinfo.yres / 2)) && !p2[first];
          first++);

        if(first < (vinfo.xres * vinfo.yres / 2)) { // ANY change?
          // Find last nonzero lword
          for(last=(vinfo.xres * vinfo.yres / 2) - 1; (last > first) &&
            !p2[last]; last--);

          int firstRow = first / (vinfo.xres / 2);
          int lastRow  = last  / (vinfo.xres / 2);

#if 0
          // Let's try a full-width copy (single memcpy operation)
          memcpy(
            (uint16_t *)&fbp[firstRow * vinfo.xres * 2],
            &screenbuf[bufNum][firstRow * vinfo.xres],
            vinfo.xres * (lastRow - firstRow + 1) * 2);
#else
          // Find actual rect bounds and use memcpy-per-row
          int firstCol = vinfo.xres - 1, lastCol = 0;
          for(int row=firstRow; row <= lastRow; row++) {
            uint16_t *s = (uint16_t *)&p2[row * vinfo.xres / 2];
            for(int col=0; col<vinfo.xres; col++) {
              if(s[col]) {
                if(col < firstCol) firstCol = col;
                if(col > lastCol)  lastCol  = col;
              }
            }
          }
          // printf("(%d,%d) (%d,%d)\n", firstCol, firstRow, lastCol, lastRow);

          int bytes = (lastCol - firstCol + 1) * 2;
          uint16_t
            *src = &screenbuf[bufNum][firstRow * vinfo.xres + firstCol],
            *dst = (uint16_t *)&fbp[(firstRow * vinfo.xres + firstCol) * 2];
          for(int row=firstRow; row <= lastRow; row++) {
            memcpy(dst, src, bytes);
            src += vinfo.xres;
            dst += vinfo.xres;
          }
//          memcpy(fbp, screenbuf[2], vinfo.xres * vinfo.yres * 2);
#endif
          bufNum = 1 - bufNum;
        }
        usleep(1000000 / 30); // 30 fps max
    }

    munmap(fbp, finfo.smem_len);
    close(fbfd);
    ret = vc_dispmanx_resource_delete(screen_resource);
    vc_dispmanx_display_close(display);
}

int main(int argc, char **argv) {
    setlogmask(LOG_UPTO(LOG_DEBUG));
    openlog("fbcp", LOG_NDELAY | LOG_PID, LOG_USER);

    return process();
}



