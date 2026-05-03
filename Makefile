REGEX_SRC = ../regex/regex/regex.c
REGEX_DEP = $(wildcard $(REGEX_SRC))

sc: source_control.c regex.c $(REGEX_DEP)
	@[ ! -f "$(REGEX_SRC)" ] || cmp -s "$(REGEX_SRC)" regex.c || cp "$(REGEX_SRC)" regex.c
	cc -O3 -Wall -Wextra -o sc source_control.c regex.c

clean:
	rm -f sc
