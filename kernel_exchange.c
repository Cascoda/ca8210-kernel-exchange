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

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/time.h>
#include <sys/select.h>

#include "cascoda_api.h"
#include "kernel_exchange.h"

/******************************************************************************/

#define DebugFSMount            "/sys/kernel/debug"
#define DriverNode              "/ca8210"
#define DriverFilePath 			(DebugFSMount DriverNode)

#define USE_LOGFILE 1

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
static pthread_mutex_t tx_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t buf_queue_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t unhandled_sync_cond = PTHREAD_COND_INITIALIZER;
static int unhandled_sync_count = 0;
static fd_set rx_block_fd_set;

#ifdef USE_LOGFILE
static FILE * LogFileDescriptor;
static pthread_mutex_t file_mutex = PTHREAD_MUTEX_INITIALIZER;
#endif

static kernel_exchange_errorhandler errorcallback;

/******************************************************************************/

struct buffer_queue{
	size_t len;
	uint8_t * buf;
	struct buffer_queue * next;
};

static struct buffer_queue * head_buffer_queue = NULL;

//Add a buffer to the FIFO queue - this is a blocking function
static void add_to_queue(const uint8_t *buf, size_t len);

//Retrieve a buffer into destBuf - this is a nonblocking function and will return 0 if nothing is retrieved from the queue
static size_t pop_from_queue(uint8_t * destBuf, size_t maxlen);

/******************************************************************************/

static void writeTime(FILE * stream){
	struct timeval tv;
	char timeString[40];

	gettimeofday(&tv, NULL);
	strftime(timeString, sizeof(timeString), "%Y-%m-%d %H:%M:%S", localtime(&tv.tv_sec));
	fprintf(stream, "\r\nat: %s.%06d ", timeString, (uint32_t)tv.tv_usec);
}

static void *ca8210_test_int_read_worker(void *arg)
{
	uint8_t rx_buf[512];
	size_t rx_len;
	int i;
	struct timeval timeout;
	timeout.tv_sec = 5;
	timeout.tv_usec = 0;

	/* TODO: while not told to exit? */
	while (1) {

		rx_len = 0;

		FD_ZERO(&rx_block_fd_set);
		FD_SET(DriverFileDescriptor, &rx_block_fd_set);

		//Wait until there is data available to read (or time out after 5 seconds)
		int rval = select(DriverFileDescriptor + 1, &rx_block_fd_set, NULL, NULL, &timeout);

		//try to get fresh data
		if(pthread_mutex_trylock(&rx_mutex) == 0){
			rx_len = read(DriverFileDescriptor, rx_buf, 0);

#ifdef USE_LOGFILE
			if (rx_len > 0) {
				pthread_mutex_lock(&file_mutex);
				writeTime(LogFileDescriptor);
				fputs("\r\nReceived Async:",LogFileDescriptor);
				for(i = 0; i < rx_len; i++){
					fprintf(LogFileDescriptor, " %02x", rx_buf[i]);
				}
				fputs("\r\n",LogFileDescriptor);
				fflush(LogFileDescriptor);
				pthread_mutex_unlock(&file_mutex);
			}
#endif

			if(rx_len > 0 && (rx_buf[0] & SPI_SYN)){	//Catch unhandled synchronous commands so synchronicity for future commands is not lost
				unhandled_sync_count--;
				assert(unhandled_sync_count >= 0);
				pthread_cond_signal(&unhandled_sync_cond);
			}

			pthread_mutex_unlock(&rx_mutex);
		}

		//If nothing was received this cycle, get something from the queue
		if(rx_len == 0){
			rx_len = pop_from_queue(rx_buf, 512);
		}

		if (rx_len > 0) {
			cascoda_downstream_dispatch(rx_buf, rx_len);
		}
	}

	return NULL;
}

int kernel_exchange_init(void){
	return kernel_exchange_init_withhandler(NULL);
}

int kernel_exchange_init_withhandler(kernel_exchange_errorhandler callback)
{
	int ret;
	static uint8_t initialised = 0;

	if(initialised) return 1;

	errorcallback = callback;
	
#ifdef USE_LOGFILE
	LogFileDescriptor = fopen("exchange.log", "a");
	fputs("\r\n-------------------NEW SESSION-------------------------\r\n",LogFileDescriptor);
	fflush(LogFileDescriptor);
#endif
	DriverFileDescriptor = open(DriverFilePath, O_RDWR | O_NONBLOCK);

	if (DriverFileDescriptor == -1) {
		fprintf(stderr, "Couldn't open driver node, errno = %d\n",
			errno);
		return -1;
	}

	cascoda_api_downstream = ca8210_test_int_exchange;

	//Empty the receive buffer for clean start
	pthread_mutex_lock(&rx_mutex);
	size_t rx_len;
	do{
		uint8_t scrap[512];
		rx_len = read(DriverFileDescriptor, scrap, 0);
	} while (rx_len != 0);
	pthread_mutex_unlock(&rx_mutex);

	unhandled_sync_count = 0;

	ret = pthread_create(&rx_thread, NULL, ca8210_test_int_read_worker, NULL);

	if(ret == 0) initialised = 1;
	return ret;
}

static int ca8210_test_int_write(const uint8_t *buf, size_t len)
{
	int returnvalue, remaining = len;
	int i, attempts = 0;

	pthread_mutex_lock(&tx_mutex);
	do {
		returnvalue = write(DriverFileDescriptor, buf+len-remaining, remaining);
		if (returnvalue > 0)
			remaining -= returnvalue;

		if(returnvalue == -1){
			int error = errno;

			if(errno == EAGAIN){	//If the error is that the device is busy, try again after a short wait
				if(attempts++ < 5){
					struct timespec toSleep;
					toSleep.tv_sec = 0;
					toSleep.tv_nsec = 50*1000000;
					nanosleep(&toSleep, NULL);	//Sleep for ~50ms
					continue;
				}
			}

			pthread_mutex_unlock(&tx_mutex);
			return error;
		}

	} while (remaining > 0);

#ifdef USE_LOGFILE
	pthread_mutex_lock(&file_mutex);
	writeTime(LogFileDescriptor);
	fputs("\r\nWriting data:  ",LogFileDescriptor);
	for(i = 0; i < len; i++){
		fprintf(LogFileDescriptor, " %02x", buf[i]);
	}
	fputs("\r\n",LogFileDescriptor);
	fflush(LogFileDescriptor);
	pthread_mutex_unlock(&file_mutex);
#endif

	pthread_mutex_unlock(&tx_mutex);
	return 0;
}

static int ca8210_test_int_exchange(
	const uint8_t *buf,
	size_t len,
	uint8_t *response,
	void *pDeviceRef
)
{
	int Rx_Length, error, i;
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

	error = ca8210_test_int_write(buf, len);
	if(error){
		//Revert all state changes and release mutexes
		if(isSynchronous)pthread_mutex_unlock(&rx_mutex);
		else if(buf[0] & SPI_SYN){
			pthread_mutex_lock(&rx_mutex);
			unhandled_sync_count--;
			pthread_mutex_unlock(&rx_mutex);
		}

		//Call the errorcallback or crash the program
		if(errorcallback) errorcallback(error);
		else abort();

		//Return a failure to the next highest layer
		return -1;
	}

	if (isSynchronous) {
		do {
			Rx_Length = read(DriverFileDescriptor, response, (size_t) 0);
			
#ifdef USE_LOGFILE
			if (Rx_Length > 0) {
				pthread_mutex_lock(&file_mutex);
				writeTime(LogFileDescriptor);
				fputs("\r\nReceived  Sync:",LogFileDescriptor);
				for(i = 0; i < Rx_Length; i++){
					fprintf(LogFileDescriptor, " %02x", response[i]);
				}
				fputs("\r\n",LogFileDescriptor);
				fflush(LogFileDescriptor);
				pthread_mutex_unlock(&file_mutex);
			}
#endif

			if(Rx_Length > 0 && !(response[0] & SPI_SYN)){
				//Unexpected asynchronous response
				add_to_queue(response, Rx_Length);
				Rx_Length = 0;
			}

		} while (Rx_Length == 0);
		pthread_mutex_unlock(&rx_mutex);
	}


	return 0;
}

static void add_to_queue(const uint8_t *buf, size_t len){
	pthread_mutex_lock(&buf_queue_mutex);
	{
		struct buffer_queue * nextbuf = head_buffer_queue;
		if(nextbuf == NULL){
			//queue empty -> start new queue
			head_buffer_queue = malloc(sizeof(struct buffer_queue));
			memset(head_buffer_queue, 0, sizeof(struct buffer_queue));
			nextbuf = head_buffer_queue;
		}
		else{
			while(nextbuf->next != NULL){
				nextbuf = nextbuf->next;
			}
			//allocate new buffer cell
			nextbuf->next = malloc(sizeof(struct buffer_queue));
			memset(nextbuf->next, 0, sizeof(struct buffer_queue));
			nextbuf = nextbuf->next;
		}

		nextbuf->len = len;
		nextbuf->buf = malloc(len);
		memcpy(nextbuf->buf, buf, len);

	}
	pthread_mutex_unlock(&buf_queue_mutex);
}

static size_t pop_from_queue(uint8_t * destBuf, size_t maxlen){

	if(pthread_mutex_trylock(&buf_queue_mutex) == 0){

		struct buffer_queue * current = head_buffer_queue;
		size_t len = 0;

		if(head_buffer_queue != NULL){
			head_buffer_queue = current->next;
			len = current->len;

			if(len > maxlen) len = 0;

			memcpy(destBuf, current->buf, len);

			free(current->buf);
			free(current);
		}

		pthread_mutex_unlock(&buf_queue_mutex);
		return len;
	}
	return 0;

}
