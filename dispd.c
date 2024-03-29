
/*
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* Copyright (c) 2010-2012 Freescale Semiconductor, Inc. */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <pthread.h>

#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>

#include <cutils/config_utils.h>
#include <cutils/cpu_info.h>
#include <cutils/properties.h>
#include <cutils/sockets.h>

#include <linux/netlink.h>

#include "dispd.h"
#include "dispmgr.h"
#include "dvi_detection.h"
#include "hdmi_detection.h"
#include "sii902x_detection.h"
#include "fb_detection.h"

#define DISPD_SOCKET "dispd"

/*
 * Globals
 */

static int ver_major = 1;
static int ver_minor = 0;
static pthread_mutex_t write_mutex = PTHREAD_MUTEX_INITIALIZER;
static int fw_sock = -1;

int bootstrap = 0;

int main(int argc, char **argv)
{
    int door_sock = -1;
    int uevent_sock = -1;
    struct sockaddr_nl nladdr;
    int uevent_sz = 64 * 1024;

    LOGI("Android Display Daemon version %d.%d", ver_major, ver_minor);

    /*
     * Create all the various sockets we'll need
     */

    // Socket to listen on for incomming framework connections
    if ((door_sock = android_get_control_socket(DISPD_SOCKET)) < 0) {
        LOGE("Obtaining file descriptor socket '%s' failed: %s",
             DISPD_SOCKET, strerror(errno));
        exit(1);
    }

    if (listen(door_sock, 4) < 0) {
        LOGE("Unable to listen on fd '%d' for socket '%s': %s", 
             door_sock, DISPD_SOCKET, strerror(errno));
        exit(1);
    }

    mkdir("/dev/block/dispd", 0755);

    // Socket to listen on for uevent changes
    memset(&nladdr, 0, sizeof(nladdr));
    nladdr.nl_family = AF_NETLINK;
    nladdr.nl_pid = getpid();
    nladdr.nl_groups = 0xffffffff;

    if ((uevent_sock = socket(PF_NETLINK,
                             SOCK_DGRAM,NETLINK_KOBJECT_UEVENT)) < 0) {
        LOGE("Unable to create uevent socket: %s", strerror(errno));
        exit(1);
    }

    if (setsockopt(uevent_sock, SOL_SOCKET, SO_RCVBUFFORCE, &uevent_sz,
                   sizeof(uevent_sz)) < 0) {
        LOGE("Unable to set uevent socket options: %s", strerror(errno));
        exit(1);
    }

    if (bind(uevent_sock, (struct sockaddr *) &nladdr, sizeof(nladdr)) < 0) {
        LOGE("Unable to bind uevent socket: %s", strerror(errno));
        exit(1);
    }

    /*
     * Bootstrap 
     */

    bootstrap = 1;

	fb_detection_bootstrap();

    bootstrap = 0;
    /*
     * Main loop
     */
    LOG_DISP("Bootstrapping complete");
    while(1) {
        fd_set read_fds;
        struct timeval to;
        int max = 0;
        int rc = 0;

        to.tv_sec = (60 * 60);
        to.tv_usec = 0;

        FD_ZERO(&read_fds);
        FD_SET(door_sock, &read_fds);
        if (door_sock > max)
            max = door_sock;
        FD_SET(uevent_sock, &read_fds);
        if (uevent_sock > max)
            max = uevent_sock;

        if (fw_sock != -1) {
            FD_SET(fw_sock, &read_fds);
            if (fw_sock > max)
                max = fw_sock;
        }

        if ((rc = select(max + 1, &read_fds, NULL, NULL, &to)) < 0) {
            LOGE("select() failed (%s)", strerror(errno));
            sleep(1);
            continue;
        }

        if (!rc) {
            continue;
        }

        if (FD_ISSET(door_sock, &read_fds)) {
            struct sockaddr addr;
            socklen_t alen;

            alen = sizeof(addr);

            if (fw_sock != -1) {
                LOGE("Dropping duplicate framework connection");
                int tmp = accept(door_sock, &addr, &alen);
                close(tmp);
                continue;
            }

            if ((fw_sock = accept(door_sock, &addr, &alen)) < 0) {
                LOGE("Unable to accept framework connection (%s)",
                     strerror(errno));
            }
            LOG_DISP("Accepted connection from framework");
            //Send the states whether the disp already been connected through fw_sock
            if ((rc = dispmgr_send_status()) < 0) {
                LOGE("Unable to send dispmgr status to framework (%d)", rc);
            }
        }

        if (FD_ISSET(fw_sock, &read_fds)) {
            LOG_DISP("process cmd from framework");
            if ((rc = process_framework_command(fw_sock)) < 0) {
                if (rc == -ECONNRESET) {
                    LOGE("Framework disconnected");
                    close(fw_sock);
                    fw_sock = -1;
                } else {
                    LOGE("Error processing framework command (%s)",
                         strerror(errno));
                }
            }
        }

        if (FD_ISSET(uevent_sock, &read_fds)) {
            LOG_DISP("process uevent from kernel");
            if ((rc = process_uevent_message(uevent_sock)) < 0) {
                LOGE("Error processing uevent msg (%s)", strerror(errno));
            }
        }
    } // while

}

int send_msg(char* message)
{
    int result = -1;

    pthread_mutex_lock(&write_mutex);
    if (fw_sock >= 0){
        LOGI("send_msg(%s):", message);
        result = write(fw_sock, message, strlen(message) + 1);
    }

    pthread_mutex_unlock(&write_mutex);

    return result;
}

int send_msg_with_data(char *message, char *data)
{
    int result = -1;

    char* buffer = (char *)alloca(strlen(message) + strlen(data) + 1);
    if (!buffer) {
        LOGE("alloca failed in send_msg_with_data");
        return -1;
    }

    strcpy(buffer, message);
    strcat(buffer, data);
    return send_msg(buffer);
}

int send_msg_with_code(int code, char *message, int fbid)
{
    char *buf;
    const char* fmt;
    const char* arg;
    char tmp[1];
    int  len;
    char fb[2];
    if(fbid >= 0){
        fmt = "%.3d %s %s";
        fb[0] =  fbid + 48;
        fb[1] = '\0';
        arg = fb;
    }else {
        fmt = "%.3d %s";
        arg = NULL;
    }
    
    /* Measure length of required buffer */
    len = snprintf(tmp, sizeof tmp, fmt, code, message, arg);
    /* Allocate in the stack, then write to it */
    buf = (char*)alloca(len+1);
    snprintf(buf, len+1, fmt, code, message, arg);
    /* Send the zero-terminated message */
    return send_msg(buf);
}
