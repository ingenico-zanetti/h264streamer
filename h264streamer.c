// data come from stdin
// they are parsed to get the h264 frames
// and when a TCP connection occurs
// the TCP client is feed with data starting at a GOP beginning
//

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <linux/limits.h>
#include <sys/time.h>
#include <sys/ioctl.h>
// #include <linux/tcp.h>
#include <netinet/tcp.h>

int listenSocket(struct in_addr *address, unsigned short port){
	struct sockaddr_in local_sock_addr = {.sin_family = AF_INET, .sin_addr = *address, .sin_port = port};
	int listen_socket = socket(AF_INET, SOCK_STREAM, 0);
	// fprintf(stderr, "listen_socket=%d" "\n", listen_socket);
	const int enable = 1;
	if (setsockopt(listen_socket, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) < 0){
		// fprintf(stderr,"setsockopt(SO_REUSEADDR) failed" "\n");
	}else{
		int error = bind(listen_socket, (struct sockaddr *)&local_sock_addr, sizeof(local_sock_addr));
		if(error){
			close(listen_socket);
			listen_socket = -1;
		}
	}
	if(listen_socket >= 0){
		int error = listen(listen_socket, 1);
		if(error){
			close(listen_socket);
			listen_socket = -2;
		}
	}
	return listen_socket;
}

#define BUFFER_SIZE (16 * 1000 * 1000)
#define MAX_OUTPUTS (16)

typedef enum {
	OUTPUT_STATE_IDLE,
	OUTPUT_STATE_RUNNING
} OutputState_e;

typedef struct {
	int fd;
	OutputState_e state;
} Output_s;

typedef struct {
	int index;
	Output_s outputs[MAX_OUTPUTS];
	uint8_t *outputBuffer;
	ssize_t outputBufferIndex;
} parserContext_s;

static void contextInitialize(parserContext_s *context){
	context->index = 0;
	int i = MAX_OUTPUTS;
	while(i--){
		context->outputs[i].fd = -1;
		context->outputs[i].state = OUTPUT_STATE_IDLE;
	}
	context->outputBuffer = (uint8_t *)malloc(BUFFER_SIZE);
	context->outputBufferIndex = 0;
}

static int contextFirstSlotAvailable(parserContext_s *context){
	int i = 0;
	while(i < MAX_OUTPUTS){
		if(-1 == context->outputs[i].fd){
			return(i);
		}
		i++;
	}
	return(-1);
}

static int printfDate(const char *formatString, char *outString, int outLen){
           time_t t;
           struct tm *tmp;

           t = time(NULL);
           tmp = localtime(&t);
           if (tmp == NULL) {
               perror("localtime");
               return(errno);
           }

           if (strftime(outString, outLen, formatString, tmp) == 0) {
               fprintf(stderr, "strftime returned 0");
               return(errno);
           }
	   printf("%s", outString);
	   return(0);
}

void printDate(void){
	char tempDate[32];
	printfDate("%Y%m%d-%H%M%S: ", tempDate, sizeof(tempDate) - 1);
}

static void analyze_and_forward(parserContext_s *context, const uint8_t *buffer, ssize_t length){
	ssize_t i = length;
	const uint8_t *p = buffer;
	while(i--){
		int doFlush = 0;
		int newGOP = 0;
		uint8_t octet = *p++;
		if((context->index < 3) && (0 == octet)){
			context->index++;
		}else if((context->index == 3) && (1 == octet)){
			context->index++;
		}else if(context->index == 4){
			// printDate(); printf("0x00000001%02X found" "\n", octet);
			context->index = 0;
			if(0x67 == octet){
				newGOP = 1;
			}
			doFlush = 1;
		}else{
			context->index = 0;
		}
		if(context->outputBufferIndex < BUFFER_SIZE){
			context->outputBuffer[context->outputBufferIndex++] = octet;
		}else{
			printDate(); printf("discard buffer, outputBufferIndex=%u, index=%u" "\n", context->outputBufferIndex, context->index = 0);
			context->outputBufferIndex  = 0;
			context->index = 0;
			doFlush = 0;
		}
		if(doFlush){
			ssize_t lengthToFlush = context->outputBufferIndex - 5;
			if(lengthToFlush > 0){
				int i = MAX_OUTPUTS;
				while(i--){
					int fd = context->outputs[i].fd;
					if(fd != -1){
						if(newGOP && (OUTPUT_STATE_IDLE == context->outputs[i].state)){
							context->outputs[i].state = OUTPUT_STATE_RUNNING;
						}
						if(OUTPUT_STATE_RUNNING == context->outputs[i].state){
							int written = write(fd, context->outputBuffer, lengthToFlush);
							if(-1 == written){
								printDate(); printf("slot %d had an error (%s), closing" "\n", i, strerror(errno));
								close(fd);
								context->outputs[i].fd  = -1;
							}else{
								if(lengthToFlush != written){
									printDate(); printf("slot %d couldn't handle the throuhput (%d < %d), wait for next key frame" "\n", i, written, lengthToFlush);
									context->outputs[i].state = OUTPUT_STATE_IDLE;
								}
							}
						}
					}
				}
				// Move current "tag" to start of buffer
				// by moving nothing
				// printf("flushed %d, reset index to 5" "\n", lengthToFlush);
				context->outputBufferIndex = 5;
				context->outputBuffer[4] = octet;
			}else{
				printDate(); printf("nothing to flush" "\n");
			}
		}
	}
}

int main(int argc, const char *argv[]){
	int in  = STDIN_FILENO;
	uint8_t *buffer = (uint8_t *)malloc(BUFFER_SIZE);
	if(buffer != NULL){
		struct in_addr listenAddress = {0}; // bind to this address for score connections
		int listeningSocket = listenSocket(&listenAddress, htons(56789));
		parserContext_s context;
		contextInitialize(&context);
		for(;;){
			void updateMax(int *m, int n){
				int max = *m;
				if(n > max){
					*m = n;
				}
			}
			int max = -1;
			fd_set fds;
			FD_ZERO(&fds);
			FD_SET(STDIN_FILENO, &fds); updateMax(&max, STDIN_FILENO);
			if(-1 != listeningSocket && (0 <= contextFirstSlotAvailable(&context))){
				FD_SET(listeningSocket, &fds); updateMax(&max, listeningSocket);
			}
			int i = MAX_OUTPUTS;
			while(i--){
				int fd = context.outputs[i].fd;
				if(-1 != fd){
					FD_SET(fd, &fds); updateMax(&max, fd);
				}
			}
			struct timeval timeout = {.tv_sec = 1, .tv_usec = 0};
			int selected = select(max + 1, &fds, NULL, NULL, &timeout);
			if(selected > 0){
				// Check forwarding socket for error (read ready should not happen)
				i = MAX_OUTPUTS;
				while(i--){
					int fd = context.outputs[i].fd;
					if(FD_ISSET(fd, &fds)){
						close(fd);
						context.outputs[i].fd = -1;
						context.outputs[i].state = OUTPUT_STATE_IDLE;
						printDate(); printf("slot %d had an error, closing fd %d" "\n", i, fd);
					}
				}
				// Check listening socket for incoming connection
				if(FD_ISSET(listeningSocket, &fds)){
					int index  = contextFirstSlotAvailable(&context);
					context.outputs[index].state = OUTPUT_STATE_IDLE;
					context.outputs[index].fd = accept(listeningSocket, NULL, NULL);
					int nonBlocking = 1;
					int rcNonBlocking = ioctl(context.outputs[index].fd, FIONBIO, &nonBlocking);
					printDate(); printf("accepted connexion to slot %d, FIONBIO=>%d" "\n", index, rcNonBlocking);
				}
				if(FD_ISSET(STDIN_FILENO, &fds)){
					// printf("data available on stdin" "\n");
					ssize_t lus = read(in, buffer, BUFFER_SIZE);
					if(lus <= 0){
						break;
					}
					analyze_and_forward(&context, buffer, lus);
				}
				fflush(stdout);
			}
		}
		free(buffer);
	}
	return(0);
}


