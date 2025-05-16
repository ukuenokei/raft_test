TARGET	= leader follower
SRCS 	= leader.c follower.c
OBJS 	= $(SRCS:.c=.o)
CC  	= gcc
CFLAGS	= -g


# $(TARGET): $(OBJS)
# 	$(CC) -o $@ $^ $(LIBDIR)$(LIBS)

all: $(TARGET)

leader: leader.c
	$(CC) $(CFLAGS) -o $@ $^ $(LIBDIR)$(LIBS)

follower: follower.c
	$(CC)  $(CFLAGS) -o $@ $^ $(LIBDIR)$(LIBS)


clean:
	rm -f $(OBJS) $(TARGET) *~