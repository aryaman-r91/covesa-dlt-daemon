/*
 * SPDX license identifier: MPL-2.0
 *
 * Copyright (C) 2011-2015, BMW AG
 *
 * This file is part of COVESA Project DLT - Diagnostic Log and Trace.
 *
 * This Source Code Form is subject to the terms of the
 * Mozilla Public License (MPL), v. 2.0.
 * If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * For further information see http://www.covesa.org/.
 */

/*!
 * \author
 * Alexander Wenzel <alexander.aw.wenzel@bmw.de>
 * Markus Klein <Markus.Klein@esk.fraunhofer.de>
 * Mikko Rapeli <mikko.rapeli@bmw.de>
 *
 * \copyright Copyright © 2011-2015 BMW AG. \n
 * License MPL-2.0: Mozilla Public License version 2.0 http://mozilla.org/MPL/2.0/.
 *
 * \file dlt_daemon_socket.c
 */


#include <netdb.h>
#include <ctype.h>
#include <stdio.h>      /* for printf() and fprintf() */
#include <sys/socket.h> /* for socket(), connect(), (), and recv() */
#include <sys/uio.h>    /* for writev() */
#include <arpa/inet.h>  /* for sockaddr_in and inet_addr() */
#include <stdlib.h>     /* for atoi() and exit() */
#include <string.h>     /* for memset() */
#include <unistd.h>     /* for close() */
#include <signal.h>
#include <syslog.h>
#include <errno.h>
#include <pthread.h>
#include <sys/ioctl.h>

#ifdef linux
#include <sys/timerfd.h>
#endif
#include <sys/time.h>
#if defined(linux) && defined(__NR_statx)
#include <linux/stat.h>
#endif

#ifdef DLT_SYSTEMD_WATCHDOG_ENABLE
#include <systemd/sd-daemon.h>
#endif

#include "dlt_types.h"
#include "dlt-daemon.h"
#include "dlt-daemon_cfg.h"
#include "dlt_daemon_common_cfg.h"

#include "dlt_daemon_socket.h"

int dlt_daemon_socket_open(int *sock, unsigned int servPort, char *ip)
{
    int yes = 1;
    int ret_inet_pton = 1;
    int lastErrno = 0;

#ifdef DLT_USE_IPv6

    /* create socket */
    if ((*sock = socket(AF_INET6, SOCK_STREAM, 0)) == -1) {
        lastErrno = errno;
        dlt_vlog(LOG_ERR, "dlt_daemon_socket_open: socket() error %d: %s\n", lastErrno,
                 strerror(lastErrno));
        return -1;
    }

#else

    if ((*sock = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        lastErrno = errno;
        dlt_vlog(LOG_ERR, "dlt_daemon_socket_open: socket() error %d: %s\n", lastErrno,
                 strerror(lastErrno));
        return -1;
    }

#endif

    dlt_vlog(LOG_INFO, "%s: Socket created\n", __FUNCTION__);

    /* setsockpt SO_REUSEADDR */
    if (setsockopt(*sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) == -1) {
        lastErrno = errno;
        dlt_vlog(
            LOG_ERR,
            "dlt_daemon_socket_open: Setsockopt error %d in dlt_daemon_local_connection_init: %s\n",
            lastErrno,
            strerror(lastErrno));
        return -1;
    }

    /* bind */
#ifdef DLT_USE_IPv6
    struct sockaddr_in6 forced_addr;
    memset(&forced_addr, 0, sizeof(forced_addr));
    forced_addr.sin6_family = AF_INET6;
    forced_addr.sin6_port = htons(servPort);

    if (0 == strcmp(ip, "0.0.0.0")) {
        forced_addr.sin6_addr = in6addr_any;
    } else {
        ret_inet_pton = inet_pton(AF_INET6, ip, &forced_addr.sin6_addr);
    }

#else
    struct sockaddr_in forced_addr;
    memset(&forced_addr, 0, sizeof(forced_addr));
    forced_addr.sin_family = AF_INET;
    forced_addr.sin_port = htons(servPort);
    ret_inet_pton = inet_pton(AF_INET, ip, &forced_addr.sin_addr);
#endif

    /* inet_pton returns 1 on success */
    if (ret_inet_pton != 1) {
        lastErrno = errno;
        dlt_vlog(
            LOG_WARNING,
            "dlt_daemon_socket_open: inet_pton() error %d: %s. Cannot convert IP address: %s\n",
            lastErrno,
            strerror(lastErrno),
            ip);
        return -1;
    }

    if (bind(*sock, (struct sockaddr *)&forced_addr, sizeof(forced_addr)) == -1) {
        lastErrno = errno;     /*close() may set errno too */
        close(*sock);
        dlt_vlog(LOG_WARNING, "dlt_daemon_socket_open: bind() error %d: %s\n", lastErrno,
                 strerror(lastErrno));
        return -1;
    }

    /*listen */
    dlt_vlog(LOG_INFO, "%s: Listening on ip %s and port: %u\n", __FUNCTION__, ip, servPort);

    /* get socket buffer size */
    dlt_vlog(LOG_INFO, "dlt_daemon_socket_open: Socket send queue size: %d\n",
             dlt_daemon_socket_get_send_qeue_max_size(*sock));

    if (listen(*sock, 3) < 0) {
        lastErrno = errno;
        dlt_vlog(LOG_WARNING,
                 "dlt_daemon_socket_open: listen() failed with error %d: %s\n",
                 lastErrno,
                 strerror(lastErrno));
        return -1;
    }

    return 0; /* OK */
}

int dlt_daemon_socket_close(int sock)
{
    close(sock);

    return 0;
}

int dlt_daemon_socket_send(int sock,
                           void *data1,
                           int size1,
                           void *data2,
                           int size2,
                           char serialheader)
{
    /* Use writev() to send all parts in a single system call
     * This reduces system call overhead compared to multiple send() calls
     * Performance optimization: combines up to 3 separate sends into one
     */
    struct iovec iov[3];
    int iov_count = 0;
    ssize_t total_size = 0;

    /* Build iovec array with non-null parts */
    if (serialheader) {
        iov[iov_count].iov_base = (void *)dltSerialHeader;
        iov[iov_count].iov_len = sizeof(dltSerialHeader);
        total_size += sizeof(dltSerialHeader);
        iov_count++;
    }

    if ((data1 != NULL) && (size1 > 0)) {
        iov[iov_count].iov_base = data1;
        iov[iov_count].iov_len = (size_t)size1;
        total_size += size1;
        iov_count++;
    }

    if ((data2 != NULL) && (size2 > 0)) {
        iov[iov_count].iov_base = data2;
        iov[iov_count].iov_len = (size_t)size2;
        total_size += size2;
        iov_count++;
    }

    /* Nothing to send */
    if (iov_count == 0) {
        return DLT_RETURN_OK;
    }

    /* Send all parts using writev() - single system call */
    ssize_t bytes_sent = 0;
    while (bytes_sent < total_size) {
        ssize_t ret = writev(sock, iov, iov_count);

        if (ret < 0) {
            dlt_vlog(LOG_WARNING,
                     "%s: socket send failed [errno: %d]!\n", __func__, errno);
#ifdef DLT_SYSTEMD_WATCHDOG_ENABLE
            /* notify systemd here that we are still alive */
            if (sd_notify(0, "WATCHDOG=1") < 0)
                dlt_vlog(LOG_WARNING, "%s: Could not reset systemd watchdog\n", __func__);
#endif
            return DLT_DAEMON_ERROR_SEND_FAILED;
        }

        bytes_sent += ret;

        /* Handle partial writes by adjusting iovec */
        if (bytes_sent < total_size) {
            ssize_t remaining = ret;
            for (int i = 0; i < iov_count; i++) {
                if (remaining >= (ssize_t)iov[i].iov_len) {
                    /* This buffer was fully sent */
                    remaining -= iov[i].iov_len;
                    iov[i].iov_len = 0;
                } else {
                    /* Partial send of this buffer */
                    iov[i].iov_base = (char *)iov[i].iov_base + remaining;
                    iov[i].iov_len -= remaining;
                    break;
                }
            }
        }
    }

    return DLT_RETURN_OK;
}

int dlt_daemon_socket_get_send_qeue_max_size(int sock)
{
    int n = 0;
    socklen_t m = sizeof(n);
    if (getsockopt(sock, SOL_SOCKET, SO_SNDBUF, (void *)&n, &m) < 0) {
        dlt_vlog(LOG_ERR,
                 "%s: socket get failed!\n", __func__);
        return -errno;
    }

    return n;
}

int dlt_daemon_socket_sendreliable(int sock, void *data_buffer, int message_size)
{
    int data_sent = 0;

    while (data_sent < message_size) {
        ssize_t ret = send(sock,
                           (uint8_t *)data_buffer + data_sent,
                           message_size - data_sent,
                           0);

        if (ret < 0) {
            dlt_vlog(LOG_WARNING,
                     "%s: socket send failed [errno: %d]!\n", __func__, errno);
#ifdef DLT_SYSTEMD_WATCHDOG_ENABLE
            /* notify systemd here that we are still alive
             * otherwise we might miss notifying the watchdog when
             * the watchdog interval is small and multiple timeouts occur back to back
             */
            if (sd_notify(0, "WATCHDOG=1") < 0)
                dlt_vlog(LOG_WARNING, "%s: Could not reset systemd watchdog\n", __func__);
#endif
            return DLT_DAEMON_ERROR_SEND_FAILED;
        } else {
            data_sent += ret;
        }
    }

    return DLT_DAEMON_ERROR_OK;
}
