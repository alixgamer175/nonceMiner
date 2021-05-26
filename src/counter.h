#include <string.h>

struct counter_state{
    unsigned long as_long_integer;
    int lower_digits;
    int middle_digits;
    int upper_digits;
    char buf[12];
    int length;
};

void init_counter_state(struct counter_state *state);
int counter_to_string(char *buf, struct counter_state *state);
void increment_counter(struct counter_state *state);

void lookup_4_digits(char *buf, int num);