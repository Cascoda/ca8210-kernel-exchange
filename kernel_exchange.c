/*
 * Copyright (c) 2016, Cascoda
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its contributors
 * may be used to endorse or promote products derived from this software without
 * specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>

#include "cascoda_api.h"

/******************************************************************************/

#define DebugFSMount            "/sys/kernel/debug"
#define DriverNode              "/ca8210"
#define DriverFilePath 			(DebugFSMount DriverNode)

/******************************************************************************/

static int ca8210_test_int_exchange(
	const uint8_t *buf,
	size_t len,
	uint8_t *response,
	void *pDeviceRef
);

/******************************************************************************/

static int DriverFileDescriptor;
static pthread_t rx_thread;
static pthread_mutex_t rx_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t unhandled_sync_cond = PTHREAD_COND_INITIALIZER;
static int unhandled_sync_count = 0;

/******************************************************************************/

static void *ca8210_test_int_read_worker(void *arg)
{
	uint8_t rx_buf[512];
	size_t rx_len;
	/* TODO: while not told to exit? */
	while (1) {
		pthread_mutex_lock(&rx_mutex);
		rx_len = read(DriverFileDescriptor, rx_buf, 0);

		if(rx_len > 0 && (buf[0] & SPI_SYN)){	//Catch unhandled synchronous commands so synchronicity for future commands is not lost
			unhandled_sync_count--;
			assert(unhandled_sync_count >= 0);
			pthread_cond_signal(&unhandled_sync_cond);
		}

		pthread_mutex_unlock(&rx_mutex);

		if (rx_len > 0) {
			cascoda_downstream_dispatch(rx_buf, rx_len);
		}
	}

	return NULL;
}

int kernel_exchange_init(void)
{
	int ret;
	static uint8_t initialised = 0;

	if(initialised) return 1;

	DriverFileDescriptor = open(DriverFilePath, O_RDWR);

	cascoda_api_downstream = ca8210_test_int_exchange;

	//Empty the receive buffer for clean start
	size_t rx_len;
	do{
		uint8_t scrap[512];
		rx_len = read(DriverFileDescriptor, scrap, 0);
	} while (rx_len != 0);

	unhandled_sync_count = 0;

	ret = pthread_create(&rx_thread, NULL, ca8210_test_int_read_worker, NULL);

	if(ret == 0) initialised = 1;
	return ret;
}

static void ca8210_test_int_write(const uint8_t *buf, size_t len)
{
	int returnvalue, remaining = len;

	do {
		returnvalue = write(DriverFileDescriptor, buf+len-remaining, remaining);
		if (returnvalue > 0)
			remaining -= returnvalue;
	} while (remaining > 0);
}

static int ca8210_test_int_exchange(
	const uint8_t *buf,
	size_t len,
	uint8_t *response,
	void *pDeviceRef
)
{
	int status, Rx_Length;
	const uint8_t isSynchronous = ((buf[0] & SPI_SYN) && response);

	if(isSynchronous){
		pthread_mutex_lock(&rx_mutex);	//Enforce synchronous write then read
		while(unhandled_sync_count != 0) {pthread_cond_wait(&unhandled_sync_cond, &rx_mutex);}
	}
	else if(buf[0] & SPI_SYN){
		pthread_mutex_lock(&rx_mutex);
		unhandled_sync_count++;
		pthread_mutex_unlock(&rx_mutex);
	}

	ca8210_test_int_write(buf, len);

	if (isSynchronous) {
		do {
			Rx_Length = read(DriverFileDescriptor, response, NULL);

			if(Rx_Length > 0 && !(response[0] & SPI_SYN)){
				//Unexpected asynchronous response
				//TODO: Perhaps queue this to be handled by the worker thread instead?
				cascoda_downstream_dispatch(response, Rx_Length);
				Rx_Length = 0;
			}

		} while (Rx_Length == 0);
		pthread_mutex_unlock(&rx_mutex);
	}


	return 0;
}
