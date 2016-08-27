NAME := timer
PARAMS := 

OPTS := 
LIBS := 
STD := c11

RELEASEOPTS := -O3
PROFILEOPTS := -pg -O3
DEBUGOPTS   := -g

COMPILER := cc
WERROR := -Werror
DEFAULTOPTS := -Wall -Wextra -pedantic $(WERROR) -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700
DEFAULTLIBS := 

SRC := $(shell find . -path ./obj -prune -o -iname "'.*'" -prune -o -iname \*.c -print)

clean : 
	rm -rf obj

obj/%.o : %.c makefile
	@mkdir -p obj
	@find . -path ./obj -path ./obj -prune -o -iname .svn -prune -o -type d ! -name . -exec mkdir -p ./obj/{} \;
	$(COMPILER) $(OPTS) $(RELEASEOPTS) $(DEFAULTOPTS) -std=$(STD) -MMD -MP -o $@ -c $<

obj/$(NAME) : $(SRC:%.c=obj/%.o)
	@mkdir -p obj
	$(COMPILER) $(OPTS) $(RELEASEOPTS) $(DEFAULTOPTS) -std=$(STD) -o $@ $^ $(LIBS) $(DEFAULTLIBS)
	
run : obj/$(NAME)
	@echo ./obj/$(NAME) $(PARAMS)
	@echo
	@./obj/$(NAME) $(PARAMS)

all : obj/$(NAME)

-include $(SRC:%.c=obj/%.d)

.PHONY : run all clean
.DEFAULT_GOAL := all
