all:
	gcc server.c -o server -lsqlite3 -lpthread
	gcc client.c -o client

install:
	sudo apt-get update
	sudo apt-get install sqlite3 libsqlite3-dev

