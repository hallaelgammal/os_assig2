CXX = g++
CXXFLAGS = -Wall -g
OBJS = command.o tokenizer.o

myshell: $(OBJS)
	$(CXX) $(CXXFLAGS) -o myshell $(OBJS)
%.o: %.cc command.h tokenizer.h
	$(CXX) $(CXXFLAGS) -c $<
clean:
	rm -f *.o myshell
