TARGET	= main client
SRCS 	= main.c client.c
OBJS 	= $(SRCS:.c=.o)
CC  	= gcc
CFLAGS	= -g


# $(TARGET): $(OBJS)
# 	$(CC) -o $@ $^ $(LIBDIR)$(LIBS)

all: $(TARGET)

main : main.c
	$(CC) $(CFLAGS) -o $@ $^ $(LIBDIR)$(LIBS)
client : client.c
	$(CC) $(CFLAGS) -o $@ $^ $(LIBDIR)$(LIBS)


clean:
	rm -f $(OBJS) $(TARGET) *~