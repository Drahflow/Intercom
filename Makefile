%: %.c common.h
	gcc -W -Wall -Wextra -pedantic -Werror -O4 -o $@ $< -lpulse
