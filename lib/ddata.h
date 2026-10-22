/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (c) 2026 ATG */

/**
 * @file ddata.h
 * @brief Moves a large buffer through a file descriptor in small pieces.
 *
 * A cooperative task must not block, so a big transfer is split over many
 * calls: each call moves at most DDATA_FRAGMENT_SIZE bytes and remembers its
 * progress in @p pointer.
 *
 * @code
 * static int sent;                     // 0 before the first call
 *
 * if (ddata_send(fd, image, sizeof(image), &sent) == 1)
 *         done();                      // the whole buffer is sent
 * @endcode
 */

#ifndef DDATA_H
#define DDATA_H

#define DDATA_FRAGMENT_SIZE 128 /**< Most bytes moved per call. */

/**
 * @brief Writes the next fragment of @p buffer.
 * @param fd      Destination.
 * @param buffer  The data.
 * @param size    Its size in bytes.
 * @param pointer Bytes done so far; set it to 0 before the first call.
 * @return 1 when everything is sent, 0 when more remains, -1 on error.
 */
int ddata_send(int fd, void *buffer, int size, int *pointer);

/**
 * @brief Reads the next fragment into @p buffer.
 * @param fd      Source.
 * @param buffer  Receives the data.
 * @param size    Bytes expected.
 * @param pointer Bytes done so far; set it to 0 before the first call.
 * @return 1 when the buffer is full, 0 when more remains, -1 on error.
 */
int ddata_recv(int fd, void *buffer, int size, int *pointer);

#endif /* DDATA_H */
