MYSQL_SRC="/root/mysql-5.7.8-rc"
BINLOG_SRC="/root/mysql-binlog-events-1.0.0-src"

LIB_MYSQL=$(MYSQL_SRC)/libmysql
LIB_BINLOG=$(BINLOG_SRC)/libbinlogevents

INCLUDES=-I$(LIB_BINLOG)/include -I$(LIB_BINLOG)/export

all:binlog_undo
.PHONY : all

binlog_undo.o:
	g++ $(INCLUDES) binlog_undo.cc -c -std=c++11 -o binlog_undo.o

main.o:
	g++ $(INCLUDES) main.cc -c -o main.o

binlog_undo:main.o binlog_undo.o
	g++ binlog_undo.o main.o \
    "${BINLOG_SRC}/lib/libmysqlstream.a" \
    "${LIB_BINLOG}/lib/libbinlogevents.a" \
    "${LIB_MYSQL}/libmysqlclient_r.a" \
    -lboost_program_options \
    -o binlog_undo 

clean:
	rm binlog_undo binlog_undo.o main.o