/* c-basic-offset: 4;  indent-tabs-mode: nil */
#include <Python.h>
#include <stdint.h>
#include "srl_inline.h"
#include "srl_encoder.h"
#include "srl_protocol.h"
#include "srl_buffer.h"
#include "ptable.h"

SRL_STATIC_INLINE unsigned VARINT_LEN(unsigned long x) { return 1 + (x / (1<<7)); }

SRL_STATIC_INLINE int SRL_ENTER_RECURSIVE_CALL(srl_encoder_t *enc, char *msg);
SRL_STATIC_INLINE void SRL_LEAVE_RECURSIVE_CALL(srl_encoder_t *enc);

SRL_STATIC_INLINE void srl_write_header(srl_encoder_t *enc);

SRL_STATIC_INLINE int srl_dump_long(srl_encoder_t *enc, long n);
SRL_STATIC_INLINE int srl_dump_binary(srl_encoder_t *enc, const char *, Py_ssize_t);

SRL_STATIC_INLINE int srl_dump_pyobj(srl_encoder_t *enc, PyObject *obj);
SRL_STATIC_INLINE int srl_dump_pyint(srl_encoder_t *enc, PyObject *obj, char *pos);
SRL_STATIC_INLINE int srl_dump_pyfloat(srl_encoder_t *enc, PyObject *obj, char *pos);
SRL_STATIC_INLINE int srl_dump_pystring(srl_encoder_t *enc, PyObject *obj, char *pos);
SRL_STATIC_INLINE int srl_dump_pyunicode(srl_encoder_t *enc, PyObject *obj, char *pos);
SRL_STATIC_INLINE int srl_dump_pylist(srl_encoder_t *enc, PyObject *obj, char *pos);

SRL_STATIC_INLINE int srl_track_obj(srl_encoder_t *enc, PyObject *obj);
SRL_STATIC_INLINE char *srl_find_obj(srl_encoder_t *enc, PyObject *obj);

const srl_encoder_ctor_args default_encoder_ctor_args = 
{
    SRL_F_SHARED_HASHKEYS, /* Default flags */
    0,                     /* Default max recursion depth */
};

srl_encoder_t *srl_encoder_new(const srl_encoder_ctor_args *args)
{
    srl_encoder_t *enc;

    if (!args)
        args = &default_encoder_ctor_args;

    enc = PyMem_Malloc(sizeof(*enc));
    if (!enc)
        return NULL;

    if (-1 == srl_encoder_ctor(enc, args)) {
        PyMem_Free(enc);
        return (void*)PyErr_NoMemory();
    }

    return enc;
}

void srl_encoder_delete(srl_encoder_t *enc)
{
    assert(enc);

    srl_encoder_dtor(enc);
    PyMem_Free(enc);
}

int srl_encoder_ctor(srl_encoder_t *enc, const srl_encoder_ctor_args *args)
{
    assert(enc);

    enc->buf_start = PyMem_Malloc(INITIAL_BUFFER_SIZE);
    if (!enc->buf_start)
        return -1;

    enc->buf_end = enc->buf_start + INITIAL_BUFFER_SIZE - 1;
    enc->pos = enc->buf_start;

    enc->operational_flags = 0;

    enc->flags = args->flags;
    enc->max_recursion_depth = args->max_recursion_depth;

    enc->recursion_depth = 0;

    enc->obj_seenhash = NULL;

    return 0;
}

void srl_encoder_dtor(srl_encoder_t *enc)
{
    assert(enc);

    PyMem_Free(enc->buf_start);
    if (enc->obj_seenhash)
        PTABLE_free(enc->obj_seenhash);
    enc->buf_start = enc->buf_end = enc->pos = NULL;
}

PyObject *srl_encoder_dump (srl_encoder_t *enc, PyObject *obj)
{
    assert(enc);

    if (!SRL_ENC_HAVE_OPTION(enc, 
                             SRL_F_COMPRESS_SNAPPY |
                             SRL_F_COMPRESS_SNAPPY_INCREMENTAL))
    {
        srl_write_header(enc);
        if (-1 == srl_dump_pyobj(enc, obj))
            return NULL;
    } else {
        PyErr_SetString(PyExc_NotImplementedError,
                        "Snappy compression not implemented yet");
        return NULL;
    }

    return PyString_FromStringAndSize(enc->buf_start, BUF_POS_OFS(enc));
}

void srl_write_header(srl_encoder_t *enc)
{
    /*
      <MAGIC> <VERSION-TYPE> <HEADER-SUFFIX-SIZE> <OPT-SUFFIX>
    */
    /* 4th to 8th bit are flags. Using 4th for snappy flag. FIXME needs to go in spec. */
    const uint8_t version_and_flags = 
        SRL_PROTOCOL_VERSION | (
            SRL_ENC_HAVE_OPTION(enc, SRL_F_COMPRESS_SNAPPY)
            ? SRL_PROTOCOL_ENCODING_SNAPPY
            : SRL_ENC_HAVE_OPTION(enc, SRL_F_COMPRESS_SNAPPY_INCREMENTAL)
            ? SRL_PROTOCOL_ENCODING_SNAPPY_INCREMENTAL
            : SRL_PROTOCOL_ENCODING_RAW
            );

    /* 4 byte magic string + proto version
     * + potentially uncompressed size varint
     * +  1 byte varint that indicates zero-length header */
    BUF_SIZE_ASSERT(enc, sizeof(SRL_MAGIC_STRING) + 1 + 1);
    srl_buf_cat_str_s_nocheck(enc, SRL_MAGIC_STRING);
    srl_buf_cat_char_nocheck(enc, version_and_flags);
    srl_buf_cat_char_nocheck(enc, '\0'); /* variable header length (0 right now) */
}

/*
  srl_dump_pyobj delegates to serialize:
    PyString
    PyUnicode
    PyInt
    PyBool
    PyFloat
    PyList

  At the moment we do not support structures containing:
      PyLong
      PyComplex
      PyByteArray
      PyTuple       
      PyDictionary  

  Also, we do not use:
    <REFP>,<COPY>,<ALIAS>,<OBJECT>,<OBJECTV>
    <WEAKEN>,<REGEXP>,
    <EXTEND>,<PAD>
    
*/
int srl_dump_pyobj(srl_encoder_t *enc, PyObject *obj)
{
    int ret;
    char *pos;

    enum {
        NONE,
        BOOL,INT,FLOAT,
        STRING,UNICODE,
        LIST
    } type;

    assert(enc);
    assert(obj);

    if (-1 == SRL_ENTER_RECURSIVE_CALL(enc, " while Sereal dumping object")) {
        return -1;
    }

    ret = -1;
    type = NONE;

    /*
      First we do fast exact checks for the base types.
      If those fail then we do slow subclass checks.
     */
    if (PyInt_CheckExact(obj))          type = INT;
    else if (PyFloat_CheckExact(obj))   type = FLOAT;
    else if (PyString_CheckExact(obj))  type = STRING;
    else if (PyUnicode_CheckExact(obj)) type = UNICODE;
    else if (PyList_CheckExact(obj))    type = LIST;
    else {
        if (PyBool_Check(obj))         type = BOOL; /*Bool a subclass of Int*/
        else if (PyInt_Check(obj))     type = INT;  /*so we check it first  */
        else if (PyFloat_Check(obj))   type = FLOAT;
        else if (PyString_Check(obj))  type = STRING;
        else if (PyUnicode_Check(obj)) type = UNICODE;
        else if (PyList_Check(obj))    type = LIST;
    }

    /*
      I COPY track immutable data:
        String,Unicode,Int,Bool,Float(,Tuple - not impl)

      I REFP track mutable data:
        List(,Dictionary,ByteArray - not impl)
     */

    if (type != BOOL) {
        pos = srl_find_obj(enc, obj);
        if (!pos) {
            if (-1 == srl_track_obj(enc, obj))
                goto finally;
        }
    }

    switch (type) {
        case INT:
            if (-1 == srl_dump_pyint(enc, obj, pos))
                goto finally;
            break;
        case BOOL:
            if (-1 == srl_buf_cat_char(enc, 
                                       obj == Py_True
                                       ? SRL_HDR_TRUE 
                                       : SRL_HDR_FALSE))
                goto finally;
            break;
        case STRING:
            if (-1 == srl_dump_pystring(enc, obj, pos))
                goto finally;
            break;
        case UNICODE:
            if (-1 == srl_dump_pyunicode(enc, obj, pos))
                goto finally;
            break;
        case FLOAT:
            if (-1 == srl_dump_pyfloat(enc, obj, pos))
                goto finally;
            break;
        case LIST:
            if (-1 == srl_dump_pylist(enc, obj, pos))
                goto finally;
            break;
        default:
            PyErr_Format(PyExc_NotImplementedError,
                         "srl_dump_pyobjc: %s dumping not implemented yet",
                         Py_TYPE(obj)->tp_name);
            goto finally;
    }

    ret = 0;
finally:
    SRL_LEAVE_RECURSIVE_CALL(enc);
    return ret;
}

SRL_STATIC_INLINE
int srl_dump_pyint(srl_encoder_t *enc, PyObject *obj, char *pos)
{
    long l;

    assert(enc);
    assert(obj);
    assert(PyInt_Check(obj));

    l = PyInt_AS_LONG(obj);
    if (pos) {
        /*  Only <COPY> if it saves space,
            so we store the shorter <VARINT>
         */
        unsigned long d;
        unsigned long zz;
        d = pos - enc->buf_start;

        if (l > 0) {  /* varint */
            /* each byte of VARINT encodes 7-bits 
               this test has false-negatives,
               we can miss a <COPY> that saves 1-byte
             */
            if (VARINT_LEN(l) > VARINT_LEN(d))
                return srl_buf_cat_varint(enc, SRL_HDR_COPY, d);
        } else { /* zigzag */
            zz = (l << 1) ^ (l >> (sizeof(long) * 8 - 1));
            if (VARINT_LEN(zz) > VARINT_LEN(d))
                return srl_buf_cat_varint(enc, SRL_HDR_COPY, d);
        }
    }
    return srl_dump_long(enc, l);
}

SRL_STATIC_INLINE
int srl_dump_long(srl_encoder_t *enc, long n)
{
    assert(enc);

    if (n >= 0)
        return 
            n < 16 
            ? srl_buf_cat_char(enc, SRL_HDR_POS_LOW | (char)n)
            : srl_buf_cat_varint(enc, SRL_HDR_VARINT, n);
    else 
        return 
            n > -17
            ? srl_buf_cat_char(enc, SRL_HDR_NEG_LOW | (char)(n + 32))
            : srl_buf_cat_zigzag(enc, SRL_HDR_ZIGZAG, n);
}

SRL_STATIC_INLINE
int srl_dump_pyfloat(srl_encoder_t *enc, PyObject *obj, char *pos)
{
    assert(enc);
    assert(obj);
    assert(PyFloat_Check(obj));

    if (pos) {
        unsigned long d;
        d = pos - enc->buf_start;
        if (VARINT_LEN(d) < sizeof(double))
            return srl_buf_cat_varint(enc, SRL_HDR_COPY, d);
    }

    return srl_buf_cat_double(enc, SRL_HDR_DOUBLE, PyFloat_AS_DOUBLE(obj));
}

SRL_STATIC_INLINE
int srl_dump_pystring(srl_encoder_t *enc, PyObject *obj, char *pos)
{
    char *p;
    Py_ssize_t n;

    assert(enc);
    assert(obj);
    assert(PyString_Check(objc));

    if (-1 == PyString_AsStringAndSize(obj, &p, &n))
        return -1;

    if (pos) {
        long d;
        d = pos - enc->buf_start;
        if (VARINT_LEN(d) < (unsigned)n)
            return srl_buf_cat_varint(enc, SRL_HDR_COPY, d);
    }

    return srl_dump_binary(enc, p, n);
}

SRL_STATIC_INLINE
int srl_dump_pyunicode(srl_encoder_t *enc, PyObject *obj, char *pos)
{
    PyObject *utf8;
    char *p;
    Py_ssize_t n;
    int ret;

    assert(enc);
    assert(obj);
    assert(PyUnicode_Check(obj));

    ret = -1;
    utf8 = PyUnicode_AsUTF8String(obj);
    if (!utf8)
        goto finally;

    if (-1 == PyString_AsStringAndSize(utf8, &p, &n))
        goto finally;

    assert(p);
    assert(n >= 0);

    if (pos) {
        unsigned long d;
        d = pos - enc->buf_start;
        if (VARINT_LEN(d) < (unsigned)n) {
            ret = srl_buf_cat_varint(enc, SRL_HDR_COPY, d);
            goto finally;
        }
    }

    /* overallocate a bit sometimes */
    if (-1 == BUF_SIZE_ASSERT(enc, 1 + SRL_MAX_VARINT_LENGTH + n))
        goto finally;

    srl_buf_cat_varint_nocheck(enc, SRL_HDR_STR_UTF8, n);
    srl_buf_cat_str_nocheck(enc, p, n);

    ret = 0;
finally:
    Py_XDECREF(utf8);
    return ret;
}


SRL_STATIC_INLINE 
int srl_dump_binary(srl_encoder_t *enc, const char *p, Py_ssize_t n)
{
    assert(enc);
    assert(p);
    assert(n >= 0);

    /* overallocate a bit sometimes */
    if (-1 == BUF_SIZE_ASSERT(enc, 1 + SRL_MAX_VARINT_LENGTH + n))
        return -1;

    if (n <= SRL_MASK_SHORT_BINARY_LEN)
        srl_buf_cat_char_nocheck(enc, SRL_HDR_SHORT_BINARY_LOW | (char) n);
    else
        srl_buf_cat_varint_nocheck(enc, SRL_HDR_BINARY, n);

    srl_buf_cat_str_nocheck(enc, p, n);
    return 0;
}

SRL_STATIC_INLINE
int srl_dump_pylist(srl_encoder_t *enc, PyObject *obj, char *pos)
{
    Py_ssize_t len;
    int ret;

    assert(enc);
    assert(obj);
    assert(PyList_Check(obj));

    if (-1 == SRL_ENTER_RECURSIVE_CALL(enc, " serializing list"))
        return -1;

    ret = -1;

    len = PyList_GET_SIZE(obj);
    assert(len >= 0);
    if (pos) {
        /* <REFP> to <ARRAYREF_N> 
           or
           <REFP> one past <REFN><ARRAY>
        */
        if (len > SRL_MASK_ARRAYREF_COUNT) 
            pos++;
        SRL_SET_FBIT(*pos);
        ret = srl_buf_cat_varint(enc, SRL_HDR_REFP, pos - enc->buf_start);
        goto finally;
    }

    if (len <= SRL_MASK_ARRAYREF_COUNT) {
        /* <ARRAYREF_N> */
        if (-1 == srl_buf_cat_char(enc, SRL_HDR_ARRAYREF_LOW+(char)len))
            goto finally;
    } else {
        /* <REFN><ARRAY><COUNT-VARINT> */
        if (-1 == BUF_SIZE_ASSERT(enc, 1+1+SRL_MAX_VARINT_LENGTH))
            goto finally;
        srl_buf_cat_char_nocheck(enc, SRL_HDR_REFN);
        srl_buf_cat_varint_nocheck(enc, SRL_HDR_ARRAY, len);
    }
    /* [<ITEM-TAG> ... ] */
    {
        int i;
        for (i = 0; i < len; i++)
            if (-1 == srl_dump_pyobj(enc, PyList_GET_ITEM(obj, i)))
                goto finally;
    }

    ret = 0;
finally:
    SRL_LEAVE_RECURSIVE_CALL(enc);
    return ret;
}

SRL_STATIC_INLINE
int srl_track_obj(srl_encoder_t *enc, PyObject *obj)
{
    if (!enc->obj_seenhash)
        /* magic const from Perl module */
        if (!(enc->obj_seenhash = PTABLE_new_size(4)))
            return -1;
    return PTABLE_store(enc->obj_seenhash, obj, enc->pos);
}

SRL_STATIC_INLINE
char *srl_find_obj(srl_encoder_t *enc, PyObject *obj)
{
    if (!enc->obj_seenhash)
        return NULL;
    return PTABLE_fetch(enc->obj_seenhash, obj);
}

SRL_STATIC_INLINE
int SRL_ENTER_RECURSIVE_CALL(srl_encoder_t *enc, char *msg)
{
    assert(enc);
    assert(msg);

    if (-1 == Py_EnterRecursiveCall(msg))
        return -1;

    enc->recursion_depth++;
    if (enc->max_recursion_depth && 
        enc->recursion_depth >= enc->max_recursion_depth) {
        PyErr_Format(PyExc_RuntimeError, 
                     "Hit maximum recursion depth (%lu),"
                     "aborting serialization",
                     (unsigned long)enc->max_recursion_depth);
        return -1;
    }

    return 0;
}

SRL_STATIC_INLINE
void SRL_LEAVE_RECURSIVE_CALL(srl_encoder_t *enc)
{
    assert(enc);

    Py_LeaveRecursiveCall();
    enc->recursion_depth--;
}
