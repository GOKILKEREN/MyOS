#ifndef USER_PROGRAMS_H
#define USER_PROGRAMS_H

typedef struct {
    const char* name;
    const unsigned char* data;
    unsigned int size;
} user_prog_t;

extern const user_prog_t user_programs[];
extern const unsigned int user_program_count;

#endif
