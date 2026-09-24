#pragma once

#define __notrace __attribute__((patchable_function_entry(0)))
#define notrace __notrace