h264streamer: h264streamer.c
	$(CC) -o h264streamer h264streamer.c

install: h264streamer
	cp -vf ./h264streamer ~/bin


