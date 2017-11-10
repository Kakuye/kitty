/*
 * fontconfig.c
 * Copyright (C) 2017 Kovid Goyal <kovid at kovidgoyal.net>
 *
 * Distributed under terms of the GPL3 license.
 */

#include "data-types.h"
#include <fontconfig/fontconfig.h>
#ifndef FC_COLOR
#define FC_COLOR "color"
#endif

static inline PyObject*
pybool(FcBool x) { PyObject *ans = x ? Py_True: Py_False; Py_INCREF(ans); return ans; }

static inline PyObject*
pyspacing(int val) {
#define S(x) case FC_##x: return PyUnicode_FromString(#x)
    switch(val) { S(PROPORTIONAL); S(DUAL); S(MONO); S(CHARCELL); default: return PyUnicode_FromString("UNKNOWN"); }
#undef S
}

static inline PyObject*
pattern_as_dict(FcPattern *pat) {
    PyObject *ans = PyDict_New();
    if (ans == NULL) return NULL;
#define PS(x) PyUnicode_FromString((char*)x)
#define G(type, get, which, conv, name) { \
    type out; PyObject *p; \
    if (get(pat, which, 0, &out) == FcResultMatch) { \
        p = conv(out); if (p == NULL) { Py_CLEAR(ans); return NULL; } \
        if (PyDict_SetItemString(ans, #name, p) != 0) { Py_CLEAR(p); Py_CLEAR(ans); return NULL; } \
        Py_CLEAR(p); \
    }}
#define S(which, key) G(FcChar8*, FcPatternGetString, which, PS, key)
#define I(which, key) G(int, FcPatternGetInteger, which, PyLong_FromLong, key)
#define B(which, key) G(int, FcPatternGetBool, which, pybool, key)
#define E(which, key, conv) G(int, FcPatternGetInteger, which, conv, key)
    S(FC_FILE, path);
    S(FC_FAMILY, family);
    S(FC_STYLE, style);
    S(FC_FULLNAME, full_name);
    S(FC_POSTSCRIPT_NAME, postscript_name);
    I(FC_WEIGHT, weight);
    I(FC_SLANT, slant);
    I(FC_HINT_STYLE, hint_style);
    I(FC_INDEX, index);
    B(FC_HINTING, hinting);
    B(FC_SCALABLE, scalable);
    B(FC_OUTLINE, outline);
    B(FC_COLOR, color);
    E(FC_SPACING, spacing, pyspacing);

    return ans;
#undef PS
#undef S
#undef I
#undef B
#undef E
#undef G
}

static inline PyObject*
font_set(FcFontSet *fs) {
    PyObject *ans = PyTuple_New(fs->nfont);
    if (ans == NULL) return NULL;
    for (int i = 0; i < fs->nfont; i++) {
        PyObject *d = pattern_as_dict(fs->fonts[i]);
        if (d == NULL) { Py_CLEAR(ans); break; }
        PyTuple_SET_ITEM(ans, i, d);
    }
    return ans;
}

#define AP(func, which, in, desc) if (!func(pat, which, in)) { PyErr_Format(PyExc_ValueError, "Failed to add %s to fontconfig pattern", desc, NULL); goto end; }

static PyObject*
fc_list(PyObject UNUSED *self, PyObject *args) {
    int allow_bitmapped_fonts = 0, only_monospaced_fonts = 1;
    PyObject *ans = NULL;
    FcObjectSet *os = NULL;
    FcPattern *pat = NULL;
    FcFontSet *fs = NULL;
    if (!PyArg_ParseTuple(args, "|pp", &only_monospaced_fonts, &allow_bitmapped_fonts)) return NULL;
    pat = FcPatternCreate();
    if (pat == NULL) return PyErr_NoMemory();
    if (!allow_bitmapped_fonts) {
        AP(FcPatternAddBool, FC_OUTLINE, true, "outline");
        AP(FcPatternAddBool, FC_SCALABLE, true, "scalable");
    }
    if (only_monospaced_fonts) AP(FcPatternAddInteger, FC_SPACING, FC_MONO, "spacing");
    os = FcObjectSetBuild(FC_FILE, FC_POSTSCRIPT_NAME, FC_FAMILY, FC_STYLE, FC_FULLNAME, FC_WEIGHT, FC_WIDTH, FC_SLANT, FC_HINT_STYLE, FC_INDEX, FC_HINTING, FC_SCALABLE, FC_OUTLINE, FC_COLOR, FC_SPACING, NULL);
    if (!os) { PyErr_SetString(PyExc_ValueError, "Failed to create fontconfig object set"); goto end; }
    fs = FcFontList(NULL, pat, os);
    if (!fs) { PyErr_SetString(PyExc_ValueError, "Failed to create fontconfig font set"); goto end; }
    ans = font_set(fs);
end:
    if (pat != NULL) FcPatternDestroy(pat);
    if (os != NULL) FcObjectSetDestroy(os);
    if (fs != NULL) FcFontSetDestroy(fs);
    return ans;
}

static inline PyObject*
_fc_match(FcPattern *pat) {
    FcPattern *match = NULL;
    PyObject *ans = NULL;
    FcResult result;
    FcConfigSubstitute(NULL, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);
    match = FcFontMatch(NULL, pat, &result);
    if (match == NULL) { PyErr_SetString(PyExc_KeyError, "FcFontMatch() failed"); goto end; }
    ans = pattern_as_dict(match);
end:
    if (match) FcPatternDestroy(match);
    return ans;
}

static inline void
add_charset(PyObject *characters, FcPattern *pat) {
    FcCharSet *charset = NULL;
    if (PyUnicode_READY(characters) != 0) goto end;
    if (PyUnicode_GET_LENGTH(characters) > 0) {
        charset = FcCharSetCreate();
        if (charset == NULL) { PyErr_NoMemory(); goto end; }
        int kind = PyUnicode_KIND(characters); void *data = PyUnicode_DATA(characters);
        for (int i = 0; i < PyUnicode_GET_LENGTH(characters); i++) {
            if (!FcCharSetAddChar(charset, PyUnicode_READ(kind, data, i))) { 
                PyErr_SetString(PyExc_RuntimeError, "Failed to add character to fontconfig charset"); 
                goto end; 
            }
        }
        AP(FcPatternAddCharSet, FC_CHARSET, charset, "charset");
    }
end:
    if (charset != NULL) FcCharSetDestroy(charset);
}

static PyObject*
fc_match(PyObject UNUSED *self, PyObject *args) {
    char *family = NULL;
    int bold = 0, italic = 0, allow_bitmapped_fonts = 0;
    double size_in_pts = 0, dpi = 0;
    PyObject *characters = NULL;
    FcPattern *pat = NULL;
    PyObject *ans = NULL;

    if (!PyArg_ParseTuple(args, "|zpppdO!d", &family, &bold, &italic, &allow_bitmapped_fonts, &size_in_pts, &PyUnicode_Type, &characters, &dpi)) return NULL;
    pat = FcPatternCreate();
    if (pat == NULL) return PyErr_NoMemory();

    if (family && strlen(family) > 0) AP(FcPatternAddString, FC_FAMILY, (const FcChar8*)family, "family");
    if (!allow_bitmapped_fonts) {
        AP(FcPatternAddBool, FC_OUTLINE, true, "outline");
        AP(FcPatternAddBool, FC_SCALABLE, true, "scalable");
    }
    if (size_in_pts > 0) { AP(FcPatternAddDouble, FC_SIZE, size_in_pts, "size"); }
    if (dpi > 0) { AP(FcPatternAddDouble, FC_DPI, dpi, "dpi"); }
    if (bold) { AP(FcPatternAddInteger, FC_WEIGHT, FC_WEIGHT_BOLD, "weight"); }
    if (italic) { AP(FcPatternAddInteger, FC_SLANT, FC_SLANT_ITALIC, "slant"); }
    if (characters) add_charset(characters, pat);
    ans = _fc_match(pat);

end:
    if (pat != NULL) FcPatternDestroy(pat);
    return ans;
}

static PyObject*
fc_font(PyObject UNUSED *self, PyObject *args) {
    double size_in_pts, dpi;
    int index;
    char *path;
    PyObject *ans = NULL, *chars = NULL;
    if (!PyArg_ParseTuple(args, "ddsi|O!", &size_in_pts, &dpi, &path, &index, &PyUnicode_Type, &chars)) return NULL;
    FcPattern *pat = FcPatternCreate();
    if (pat == NULL) return PyErr_NoMemory();
    if (size_in_pts > 0) { AP(FcPatternAddDouble, FC_SIZE, size_in_pts, "size"); }
    if (dpi > 0) { AP(FcPatternAddDouble, FC_DPI, dpi, "dpi"); }
    AP(FcPatternAddString, FC_FILE, (const FcChar8*)path, "path");
    AP(FcPatternAddInteger, FC_INDEX, index, "index"); 
    if (chars) add_charset(chars, pat);
    ans = _fc_match(pat);

end:
    if (pat != NULL) FcPatternDestroy(pat);
    return ans;
}

#undef AP
static PyMethodDef module_methods[] = {
    METHODB(fc_list, METH_VARARGS),
    METHODB(fc_match, METH_VARARGS),
    METHODB(fc_font, METH_VARARGS),
    {NULL, NULL, 0, NULL}        /* Sentinel */
};

bool 
init_fontconfig_library(PyObject *module) {
    if (!FcInit()) {
        PyErr_SetString(PyExc_RuntimeError, "Failed to initialize the fontconfig library");
        return false;
    }
    if (Py_AtExit(FcFini) != 0) {
        PyErr_SetString(PyExc_RuntimeError, "Failed to register the fontconfig library at exit handler");
        return false;
    }
    if (PyModule_AddFunctions(module, module_methods) != 0) return false;
    PyModule_AddIntMacro(module, FC_WEIGHT_REGULAR);
    PyModule_AddIntMacro(module, FC_WEIGHT_MEDIUM);
    PyModule_AddIntMacro(module, FC_WEIGHT_SEMIBOLD);
    PyModule_AddIntMacro(module, FC_WEIGHT_BOLD);
    PyModule_AddIntMacro(module, FC_SLANT_ITALIC);
    PyModule_AddIntMacro(module, FC_SLANT_ROMAN);
    return true;
}
