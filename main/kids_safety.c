#include "kids_safety.h"

#include <ctype.h>
#include <string.h>

static bool contains_word_ci(const char *haystack, const char *needle)
{
    if (!haystack || !needle || !needle[0]) {
        return false;
    }

    size_t nlen = strlen(needle);
    for (const char *p = haystack; *p; p++) {
        size_t i = 0;
        while (i < nlen && p[i] &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) {
            i++;
        }
        if (i == nlen) {
            return true;
        }
    }
    return false;
}

bool kids_safety_message_allowed(const char *message, const char **blocked_reason)
{
    if (blocked_reason) {
        *blocked_reason = NULL;
    }

    if (!message || !message[0]) {
        if (blocked_reason) {
            *blocked_reason = "empty_message";
        }
        return false;
    }

    static const char *blocked[] = {
        "password", "address", "phone", "secret", "kill", "weapon",
        "bomb", "drug", "suicide", "sex", "politics", "medical",
        "lawyer", "credit card", "bank",
        "lakcim", "cimem", "telefon", "jelszo", "titok", "fegyver",
        "bomba", "drog", "ongyilkos", "politika", "orvos", "gyogyszer",
        NULL
    };

    for (int i = 0; blocked[i]; i++) {
        if (contains_word_ci(message, blocked[i])) {
            if (blocked_reason) {
                *blocked_reason = "local_blocklist";
            }
            return false;
        }
    }

    return true;
}
