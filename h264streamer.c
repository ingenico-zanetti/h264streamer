// data come from stdin
// they are parsed to get the h264 frames
// and when a TCP connection occurs
// the TCP client is feed with data starting at a GOP beginning
//

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>


#define BUFFER_SIZE (1 * 1000 * 1000)
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
			printf("0x00000001%02X found" "\n", octet);
			context->index = 0;
			if(0x67 == octet){
				newGOP = 1;
			}
			doFlush = 1;
		}else{
			context->index = 0;
		}
		context->outputBuffer[context->outputBufferIndex++] = octet;
		if(doFlush){
			ssize_t lengthToFlush = context->outputBufferIndex - 5;
			if(lengthToFlush > 0){
				int i = MAX_OUTPUTS;
				while(i--){
					if(context->outputs[i].fd != -1){
						if(newGOP && (OUTPUT_STATE_IDLE == context->outputs[i].state)){
							context->outputs[i].state = OUTPUT_STATE_RUNNING;
						}
						if(OUTPUT_STATE_RUNNING == context->outputs[i].state){
							if(lengthToFlush != write(context->outputs[i].fd, context->outputBuffer, lengthToFlush)){
								context->outputs[i].state = OUTPUT_STATE_IDLE;
								close(context->outputs[i].fd);
								context->outputs[i].fd  = -1;
							}
						}
					}
				}
				// Move current "tag" to start of buffer
				// by moving nothing
				printf("flushed %d, reset index to 5" "\n", lengthToFlush);
				context->outputBufferIndex = 5;
				context->outputBuffer[4] = octet;
			}else{
				printf("nothing to flush" "\n");
			}
		}
	}
}

int main(int argc, const char *argv[]){
	int in  = STDIN_FILENO;
	uint8_t *buffer = (uint8_t *)malloc(BUFFER_SIZE);
	if(buffer != NULL){
		parserContext_s context;
		contextInitialize(&context);
		for(;;){
			ssize_t lus = read(in, buffer, BUFFER_SIZE);
			if(lus <= 0){
				break;
			}
			analyze_and_forward(&context, buffer, lus);
		}
		free(buffer);
	}
	return(0);
}


