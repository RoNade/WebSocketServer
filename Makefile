all: websocketserver

websocketserver: main.c
	gcc -g main.c -o ./WebSocketServer -I/usr/local/include -L/usr/local/lib -lwebsockets
