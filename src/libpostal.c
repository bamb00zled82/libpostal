#include <stdlib.h>

#include "libpostal.h"

#include "klib/khash.h"
#include "klib/ksort.h"
#include "log/log.h"

#include "address_dictionary.h"
#include "address_parser.h"
#include "dedupe.h"
#include "expand.h"

#include "language_classifier.h"
#include "near_dupe.h"
#include "normalize.h"
#include "place.h"
#include "scanner.h"
#include "string_utils.h"
#include "token_types.h"

#include <ctype.h>
#include <string.h>

// Country heuristics helpers

static const char *US_STATES[] = {
    "al","ak","az","ar","ca","co","ct","de","fl","ga",
    "hi","id","il","in","ia","ks","ky","la","me","md",
    "ma","mi","mn","ms","mo","mt","ne","nv","nh","nj",
    "nm","ny","nc","nd","oh","ok","or","pa","ri","sc",
    "sd","tn","tx","ut","vt","va","wa","wv","wi","wy",
    "dc", NULL
};

static const char *CA_PROVINCES[] = {
    "ab","bc","mb","nb","nl","ns","nt","nu","on","pe",
    "qc","sk","yt", NULL
};

static const char *AU_STATES[] = {
    "nsw","vic","qld","wa","sa","tas","act","nt", NULL
};

static const struct {
    const char *name;
    const char *iso2;
} EXPLICIT_COUNTRY_MAP[] = {
    {"united states", "US"},
    {"united states of america", "US"},
    {"usa", "US"},
    {"us", "US"},

    {"united kingdom", "GB"},
    {"uk", "GB"},
    {"great britain", "GB"},
    {"england", "GB"},
    {"scotland", "GB"},
    {"wales", "GB"},

    {"canada", "CA"},
    {"ca", "CA"},

    {"australia", "AU"},
    {"au", "AU"},

    {"germany", "DE"},
    {"deutschland", "DE"},

    {"france", "FR"},
    {"india", "IN"},
    {"brazil", "BR"},
    {"japan", "JP"},
    {"china", "CN"},
};

static const size_t EXPLICIT_COUNTRY_MAP_LEN =
    sizeof(EXPLICIT_COUNTRY_MAP) / sizeof(EXPLICIT_COUNTRY_MAP[0]);

static bool in_string_set(const char *value, const char *const *set) {
    if (value == NULL) return false;
    for (size_t i = 0; set[i] != NULL; i++) {
        if (strcmp(value, set[i]) == 0) {
            return true;
        }
    }
    return false;
}

static bool is_all_digits(const char *s) {
    if (s == NULL || *s == '\0') return false;
    for (; *s; s++) {
        if (!isdigit((unsigned char)*s)) return false;
    }
    return true;
}

static bool has_alpha(const char *s) {
    if (s == NULL) return false;
    for (; *s; s++) {
        if (isalpha((unsigned char)*s)) return true;
    }
    return false;
}

// US ZIP: 5 digits or 5-4
static bool looks_like_us_zip(const char *s) {
    if (s == NULL) return false;
    size_t len = strlen(s);
    if (len == 5 && is_all_digits(s)) return true;
    if (len == 10 && s[5] == '-') {
        char buf[6];
        memcpy(buf, s, 5);
        buf[5] = '\0';
        if (!is_all_digits(buf)) return false;
        if (!is_all_digits(s + 6)) return false;
        return true;
    }
    return false;
}

// Canada: A1A 1A1 pattern-ish
static bool looks_like_ca_postcode(const char *s) {
    if (s == NULL) return false;
    // strip space for simplicity
    char buf[16];
    size_t len = 0;
    for (; *s && len < sizeof(buf) - 1; s++) {
        if (*s != ' ') {
            buf[len++] = (char)tolower((unsigned char)*s);
        }
    }
    buf[len] = '\0';
    if (len != 6) return false;
    return isalpha((unsigned char)buf[0]) &&
           isdigit((unsigned char)buf[1]) &&
           isalpha((unsigned char)buf[2]) &&
           isdigit((unsigned char)buf[3]) &&
           isalpha((unsigned char)buf[4]) &&
           isdigit((unsigned char)buf[5]);
}

// UK: very loose – has both letters and digits, length 5–8, usually ends with 2–3 letters
static bool looks_like_uk_postcode(const char *s) {
    if (s == NULL) return false;
    size_t len = strlen(s);
    if (len < 4 || len > 8) return false;
    if (!has_alpha(s)) return false;
    int digits = 0;
    for (size_t i = 0; i < len; i++) {
        if (isdigit((unsigned char)s[i])) digits++;
    }
    if (digits == 0) return false;

    while (len > 0 && s[len - 1] == ' ') len--;
    if (len >= 2 &&
        isalpha((unsigned char)s[len - 1]) &&
        isalpha((unsigned char)s[len - 2])) {
        return true;
    }
    return false;
}

// Generic mapper for explicit "country" field
static const char *map_country_name_to_iso2(const char *value) {
    if (value == NULL) return NULL;
    for (size_t i = 0; i < EXPLICIT_COUNTRY_MAP_LEN; i++) {
        if (strcmp(value, EXPLICIT_COUNTRY_MAP[i].name) == 0) {
            return EXPLICIT_COUNTRY_MAP[i].iso2;
        }
    }
    return NULL;
}

static libpostal_normalize_options_t LIBPOSTAL_DEFAULT_OPTIONS = {
        .languages = NULL,
        .num_languages = 0,
        .address_components = LIBPOSTAL_ADDRESS_NAME | LIBPOSTAL_ADDRESS_HOUSE_NUMBER | LIBPOSTAL_ADDRESS_STREET | LIBPOSTAL_ADDRESS_PO_BOX | LIBPOSTAL_ADDRESS_UNIT | LIBPOSTAL_ADDRESS_LEVEL | LIBPOSTAL_ADDRESS_ENTRANCE | LIBPOSTAL_ADDRESS_STAIRCASE | LIBPOSTAL_ADDRESS_POSTAL_CODE,
        .latin_ascii = true,
        .transliterate = true,
        .strip_accents = true,
        .decompose = true,
        .lowercase = true,
        .trim_string = true,
        .drop_parentheticals = true,
        .replace_numeric_hyphens = false,
        .delete_numeric_hyphens = false,
        .split_alpha_from_numeric = true,
        .replace_word_hyphens = true,
        .delete_word_hyphens = true,
        .delete_final_periods = true,
        .delete_acronym_periods = true,
        .drop_english_possessives = true,
        .delete_apostrophes = true,
        .expand_numex = true,
        .roman_numerals = true
};

libpostal_normalize_options_t libpostal_get_default_options(void) {
    return LIBPOSTAL_DEFAULT_OPTIONS;
}

char **libpostal_expand_address(char *input, libpostal_normalize_options_t options, size_t *n) {
    cstring_array *strings = expand_address(input, options, n);
    if (strings == NULL) return NULL;
    return cstring_array_to_strings(strings);
}

char **libpostal_expand_address_root(char *input, libpostal_normalize_options_t options, size_t *n) {
    cstring_array *strings = expand_address_root(input, options, n);
    if (strings == NULL) return NULL;
    return cstring_array_to_strings(strings);
}

void libpostal_expansion_array_destroy(char **expansions, size_t n) {
    expansion_array_destroy(expansions, n);
}

#define DEFAULT_NEAR_DUPE_GEOHASH_PRECISION 6

static libpostal_near_dupe_hash_options_t LIBPOSTAL_NEAR_DUPE_HASH_DEFAULT_OPTIONS = {
    .with_name = true,
    .with_address = true,
    .with_unit = false,
    .with_city_or_equivalent = true,
    .with_small_containing_boundaries = true,
    .with_postal_code = true,
    .with_latlon = false,
    .latitude = 0.0,
    .longitude = 0.0,
    .geohash_precision = DEFAULT_NEAR_DUPE_GEOHASH_PRECISION,
    .name_and_address_keys = true,
    .name_only_keys = false,
    .address_only_keys = false
};

libpostal_near_dupe_hash_options_t libpostal_get_near_dupe_hash_default_options(void) {
    return LIBPOSTAL_NEAR_DUPE_HASH_DEFAULT_OPTIONS;
}

char **libpostal_near_dupe_name_hashes(char *name, libpostal_normalize_options_t normalize_options, size_t *num_hashes) {
    cstring_array *strings = name_word_hashes(name, normalize_options);
    if (strings == NULL) {
        *num_hashes = 0;
        return NULL;
    }
    *num_hashes = cstring_array_num_strings(strings);
    return cstring_array_to_strings(strings);
}


char **libpostal_near_dupe_hashes(size_t num_components, char **labels, char **values, libpostal_near_dupe_hash_options_t options, size_t *num_hashes) {
    cstring_array *strings = near_dupe_hashes(num_components, labels, values, options);
    if (strings == NULL) {
        *num_hashes = 0;
        return NULL;
    }
    *num_hashes = cstring_array_num_strings(strings);
    return cstring_array_to_strings(strings);
}


char **libpostal_near_dupe_hashes_languages(size_t num_components, char **labels, char **values, libpostal_near_dupe_hash_options_t options, size_t num_languages, char **languages, size_t *num_hashes) {
    cstring_array *strings = near_dupe_hashes_languages(num_components, labels, values, options, num_languages, languages);
    if (strings == NULL) {
        *num_hashes = 0;
        return NULL;
    }
    *num_hashes = cstring_array_num_strings(strings);
    return cstring_array_to_strings(strings);
}


char **libpostal_place_languages(size_t num_components, char **labels, char **values, size_t *num_languages) {
    libpostal_language_classifier_response_t *lang_response = place_languages(num_components, labels, values);
    if (lang_response == NULL) {
        *num_languages = 0;
        return NULL;
    }

    char **languages = lang_response->languages;
    lang_response->languages = NULL;
    *num_languages = lang_response->num_languages;
    lang_response->num_languages = 0;

    language_classifier_response_destroy(lang_response);
    return languages;
}

static libpostal_duplicate_options_t LIBPOSTAL_DUPLICATE_DEFAULT_OPTIONS = {
    .num_languages = 0,
    .languages = NULL
};

libpostal_duplicate_options_t libpostal_get_default_duplicate_options(void) {
    return LIBPOSTAL_DUPLICATE_DEFAULT_OPTIONS;
}

libpostal_duplicate_options_t libpostal_get_duplicate_options_with_languages(size_t num_languages, char **languages) {
    libpostal_duplicate_options_t options = LIBPOSTAL_DUPLICATE_DEFAULT_OPTIONS;
    options.num_languages = num_languages;
    options.languages = languages;
    return options;
}

libpostal_duplicate_status_t libpostal_is_name_duplicate(char *value1, char *value2, libpostal_duplicate_options_t options) {
    return is_name_duplicate(value1, value2, options);
}

libpostal_duplicate_status_t libpostal_is_street_duplicate(char *value1, char *value2, libpostal_duplicate_options_t options) {
    return is_street_duplicate(value1, value2, options);
}

libpostal_duplicate_status_t libpostal_is_house_number_duplicate(char *value1, char *value2, libpostal_duplicate_options_t options) {
    return is_house_number_duplicate(value1, value2, options);
}

libpostal_duplicate_status_t libpostal_is_po_box_duplicate(char *value1, char *value2, libpostal_duplicate_options_t options) {
    return is_po_box_duplicate(value1, value2, options);
}

libpostal_duplicate_status_t libpostal_is_unit_duplicate(char *value1, char *value2, libpostal_duplicate_options_t options) {
    return is_unit_duplicate(value1, value2, options);
}

libpostal_duplicate_status_t libpostal_is_floor_duplicate(char *value1, char *value2, libpostal_duplicate_options_t options) {
    return is_floor_duplicate(value1, value2, options);
}

libpostal_duplicate_status_t libpostal_is_postal_code_duplicate(char *value1, char *value2, libpostal_duplicate_options_t options) {
    return is_postal_code_duplicate(value1, value2, options);
}

libpostal_duplicate_status_t libpostal_is_toponym_duplicate(size_t num_components1, char **labels1, char **values1, size_t num_components2, char **labels2, char **values2, libpostal_duplicate_options_t options) {
    return is_toponym_duplicate(num_components1, labels1, values1, num_components2, labels2, values2, options);
}

#define DEFAULT_FUZZY_DUPLICATE_NEEDS_REVIEW_THRESHOLD 0.7
#define DEFAULT_FUZZY_DUPLICATE_LIKELY_DUPE_THRESHOLD 0.9

static libpostal_fuzzy_duplicate_options_t DEFAULT_FUZZY_DUPLICATE_OPTIONS = {
    .num_languages = 0,
    .languages = NULL,
    .needs_review_threshold = DEFAULT_FUZZY_DUPLICATE_NEEDS_REVIEW_THRESHOLD,
    .likely_dupe_threshold = DEFAULT_FUZZY_DUPLICATE_LIKELY_DUPE_THRESHOLD
};


libpostal_fuzzy_duplicate_options_t libpostal_get_default_fuzzy_duplicate_options(void) {
    return DEFAULT_FUZZY_DUPLICATE_OPTIONS;
}

libpostal_fuzzy_duplicate_options_t libpostal_get_default_fuzzy_duplicate_options_with_languages(size_t num_languages, char **languages) {
    libpostal_fuzzy_duplicate_options_t options = DEFAULT_FUZZY_DUPLICATE_OPTIONS;
    options.num_languages = num_languages;
    options.languages = languages;
    return options;
}


libpostal_fuzzy_duplicate_status_t libpostal_is_name_duplicate_fuzzy(size_t num_tokens1, char **tokens1, double *token_scores1, size_t num_tokens2, char **tokens2, double *token_scores2, libpostal_fuzzy_duplicate_options_t options) {
    return is_name_duplicate_fuzzy(num_tokens1, tokens1, token_scores1, num_tokens2, tokens2, token_scores2, options);
}

libpostal_fuzzy_duplicate_status_t libpostal_is_street_duplicate_fuzzy(size_t num_tokens1, char **tokens1, double *token_scores1, size_t num_tokens2, char **tokens2, double *token_scores2, libpostal_fuzzy_duplicate_options_t options) {
    return is_street_duplicate_fuzzy(num_tokens1, tokens1, token_scores1, num_tokens2, tokens2, token_scores2, options);
}

libpostal_language_classifier_response_t *libpostal_classify_language(char *address) {
    libpostal_language_classifier_response_t *response = classify_languages(address);

    if (response == NULL) {
        log_error("Language classification returned NULL\n");
        return NULL;
    }

    return response;
}

void libpostal_language_classifier_response_destroy(libpostal_language_classifier_response_t *self) {
    if (self == NULL) return;
    if (self->languages != NULL) {
        free(self->languages);
    }

    if (self->probs) {
        free(self->probs);
    }

    free(self);
}


void libpostal_address_parser_response_destroy(libpostal_address_parser_response_t *self) {
    if (self == NULL) return;

    for (size_t i = 0; i < self->num_components; i++) {
        if (self->components != NULL) {
            free(self->components[i]);
        }

        if (self->labels != NULL) {
            free(self->labels[i]);
        }
    }

    if (self->components != NULL) {
        free(self->components);
    }

    if (self->labels != NULL) {
        free(self->labels);
    }

    // prevent memory leak
    if (self->country_guess != NULL) {
        free(self->country_guess);
    }

    free(self);
}

static libpostal_address_parser_options_t LIBPOSTAL_ADDRESS_PARSER_DEFAULT_OPTIONS =  {
    .language = NULL,
    .country = NULL
};

inline libpostal_address_parser_options_t libpostal_get_address_parser_default_options(void) {
    return LIBPOSTAL_ADDRESS_PARSER_DEFAULT_OPTIONS;
}

libpostal_address_parser_response_t *libpostal_parse_address(char *address, libpostal_address_parser_options_t options) {
    libpostal_address_parser_response_t *parsed = address_parser_parse(address, options.language, options.country);

    if (parsed == NULL) {
        log_error("Parser returned NULL\n");
        return NULL;
    }

    // HEURISTICS NEW FEATURES BELOW

    // clear any existing guess
    parsed->country_guess = NULL;

    // if caller provided options.country, that’s a strong prior
    if (options.country != NULL) {
        if (parsed->country_guess) free(parsed->country_guess);
        parsed->country_guess = strdup(options.country);
    }

    // scan parsed components for explicit country/state/postcode hints
    const char *postcode_value = NULL;

    for (size_t i = 0; i < parsed->num_components; i++) {
        char *label = parsed->labels[i];
        char *value = parsed->components[i];

        if (label == NULL || value == NULL) continue;

        // explicit country component
        if (strcmp(label, "country") == 0) {
            const char *mapped = map_country_name_to_iso2(value);
            if (parsed->country_guess) {
                free(parsed->country_guess);
                parsed->country_guess = NULL;
            }
            if (mapped != NULL) {
                parsed->country_guess = strdup(mapped);
            } else {
                // fallback: copy whatever libpostal recognized
                parsed->country_guess = strdup(value);
            }
            // explicit beats everything else – we can stop here
            break;
        }

        // state/province component
        if (strcmp(label, "state") == 0) {
            if (in_string_set(value, US_STATES)) {
                if (parsed->country_guess == NULL) {
                    parsed->country_guess = strdup("US");
                }
            } else if (in_string_set(value, CA_PROVINCES)) {
                if (parsed->country_guess == NULL) {
                    parsed->country_guess = strdup("CA");
                }
            } else if (in_string_set(value, AU_STATES)) {
                if (parsed->country_guess == NULL) {
                    parsed->country_guess = strdup("AU");
                }
            }
        }

        // remember postcode for later pattern heuristics
        if (strcmp(label, "postcode") == 0 && postcode_value == NULL) {
            postcode_value = value;
        }
    }

    // if we still have no guess, try postcode patterns
    if (parsed->country_guess == NULL && postcode_value != NULL) {
        const char *pc = postcode_value;

        if (looks_like_ca_postcode(pc)) {
            parsed->country_guess = strdup("CA");
        } else if (looks_like_us_zip(pc)) {
            parsed->country_guess = strdup("US");
        } else if (looks_like_uk_postcode(pc)) {
            parsed->country_guess = strdup("GB");
        }
        // can add more here in future
    }


    return parsed;
}

bool libpostal_parser_print_features(bool print_features) {
    return address_parser_print_features(print_features);
}

bool libpostal_setup_datadir(char *datadir) {
    char *transliteration_path = NULL;
    char *numex_path = NULL;
    char *address_dictionary_path = NULL;

    if (datadir != NULL) {
        transliteration_path = path_join(3, datadir, LIBPOSTAL_TRANSLITERATION_SUBDIR, TRANSLITERATION_DATA_FILE);
        numex_path = path_join(3, datadir, LIBPOSTAL_NUMEX_SUBDIR, NUMEX_DATA_FILE);
        address_dictionary_path = path_join(3, datadir, LIBPOSTAL_ADDRESS_EXPANSIONS_SUBDIR, ADDRESS_DICTIONARY_DATA_FILE);
    }
    
    bool setup_succeed = true;

    if (!transliteration_module_setup(transliteration_path)) {
        log_error("Error loading transliteration module, dir=%s\n", transliteration_path);
        setup_succeed = false;
    }

    if (setup_succeed && !numex_module_setup(numex_path)) {
        log_error("Error loading numex module, dir=%s\n", numex_path);
        setup_succeed = false;
    }

    if (setup_succeed && !address_dictionary_module_setup(address_dictionary_path)) {
        log_error("Error loading dictionary module, dir=%s\n", address_dictionary_path);
        setup_succeed = false;
    }

    if (transliteration_path != NULL) {
        free(transliteration_path);
    }

    if (numex_path != NULL) {
        free(numex_path);
    }

    if (address_dictionary_path != NULL) {
        free(address_dictionary_path);
    }

    return setup_succeed;
}

bool libpostal_setup(void) {
    return libpostal_setup_datadir(NULL);
}

bool libpostal_setup_language_classifier_datadir(char *datadir) {
    char *language_classifier_dir = NULL;

    if (datadir != NULL) {
        language_classifier_dir = path_join(2, datadir, LIBPOSTAL_LANGUAGE_CLASSIFIER_SUBDIR);
    }

    if (!language_classifier_module_setup(language_classifier_dir)) {
        log_error("Error loading language classifier, dir=%s\n", language_classifier_dir);
        return false;
    }

    if (language_classifier_dir != NULL) {
        free(language_classifier_dir);
    }

    return true;
}


libpostal_token_t *libpostal_tokenize(char *input, bool whitespace, size_t *n) {
    token_array *tokens = NULL;
    if (!whitespace) {
        tokens = tokenize(input);
    } else {
        tokens = tokenize_keep_whitespace(input);
    }

    if (tokens == NULL) {
        return NULL;
    }

    libpostal_token_t *a = tokens->a;
    *n = tokens->n;
    free(tokens);
    return a;
}


char *libpostal_normalize_string_languages(char *str, uint64_t options, size_t num_languages, char **languages) {
    if (options & LIBPOSTAL_NORMALIZE_STRING_LATIN_ASCII) {
        return normalize_string_latin_languages(str, strlen(str), options, num_languages, languages);
    } else {
        return normalize_string_utf8_languages(str, options, num_languages, languages);
    }
}

inline char *libpostal_normalize_string(char *str, uint64_t options) {
    return libpostal_normalize_string_languages(str, options, 0, NULL);
}

libpostal_normalized_token_t *libpostal_normalized_tokens_languages(char *input, uint64_t string_options, uint64_t token_options, bool whitespace, size_t num_languages, char **languages, size_t *n) {
    if (input == NULL) {
        return NULL;
    }
    char *normalized = libpostal_normalize_string_languages(input, string_options, num_languages, languages);
    if (normalized == NULL) {
        return NULL;
    }

    token_array *tokens = NULL;
    if (!whitespace) {
        tokens = tokenize(normalized);
    } else {
        tokens = tokenize_keep_whitespace(normalized);
    }

    if (tokens == NULL || tokens->a == NULL) {
        free(normalized);
        return NULL;
    }

    size_t num_tokens = tokens->n;
    token_t *token_array = tokens->a;
    char_array *normalized_token = char_array_new_size(strlen(normalized));

    libpostal_normalized_token_t *result = malloc(sizeof(libpostal_normalized_token_t) * num_tokens);

    for (size_t i = 0; i < num_tokens; i++) {
        token_t token = token_array[i];
        char_array_clear(normalized_token);
        add_normalized_token(normalized_token, normalized, token, token_options);
        char *token_str = strdup(char_array_get_string(normalized_token));
        result[i] = (libpostal_normalized_token_t){token_str, token};
    }

    free(normalized);
    token_array_destroy(tokens);
    char_array_destroy(normalized_token);

    *n = num_tokens;
    return result;
}

inline libpostal_normalized_token_t *libpostal_normalized_tokens(char *input, uint64_t string_options, uint64_t token_options, bool whitespace, size_t *n) {
    return libpostal_normalized_tokens_languages(input, string_options, token_options, whitespace, 0, NULL, n);
}


bool libpostal_setup_language_classifier(void) {
    return libpostal_setup_language_classifier_datadir(NULL);
}

bool libpostal_setup_parser_datadir(char *datadir) {
    char *parser_dir = NULL;

    if (datadir != NULL) {
        parser_dir = path_join(2, datadir, LIBPOSTAL_ADDRESS_PARSER_SUBDIR);
    }

    if (!address_parser_module_setup(parser_dir)) {
        log_error("Error loading address parser module, dir=%s\n", parser_dir);
        return false;
    }

    if (parser_dir != NULL) {
        free(parser_dir);
    }

    return true;
}

bool libpostal_setup_parser(void) {
    return libpostal_setup_parser_datadir(NULL);
}

void libpostal_teardown(void) {
    transliteration_module_teardown();

    numex_module_teardown();

    address_dictionary_module_teardown();
}

void libpostal_teardown_language_classifier(void) {
    language_classifier_module_teardown();
}

void libpostal_teardown_parser(void) {
    address_parser_module_teardown();
}