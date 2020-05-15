/**
 * Copyright (c) 2012 MIT License by 6.172 Staff
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 **/

#ifndef _KTIMING_H_
#define _KTIMING_H_

#include <inttypes.h>

typedef uint64_t clockmark_t;

uint64_t 
ktiming_diff_usec(const clockmark_t* const start, const clockmark_t* const end);
double
ktiming_diff_sec(const clockmark_t* const start, const clockmark_t* const end);
clockmark_t ktiming_getmark(void);
void print_runtime(uint64_t *tm_elapsed, int size); 
void print_runtime_summary(uint64_t *tm_elapsed, int size); 

#endif  // _KTIMING_H_

