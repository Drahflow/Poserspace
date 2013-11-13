CPPOPTS=-W -Wall -Wextra -Werror -pedantic --std=c++11 -O4 -ggdb
LIBS=-lSDL2 -lSDL2_image

display: main.o
	g++ $(LIBS) $(CPPOPTS) -o $@ $<

%.o: %.c++
	g++ $(CPPOPTS) -c -o $@ $<
