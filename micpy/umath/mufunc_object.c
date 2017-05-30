/*
 * Python Universal Functions Object -- Math for all types, plus fast
 * arrays math
 *
 * Full description
 *
 * This supports mathematical (and Boolean) functions on arrays and other python
 * objects.  Math on large arrays of basic C types is rather efficient.
 *
 * Travis E. Oliphant  2005, 2006 oliphant@ee.byu.edu (oliphant.travis@ieee.org)
 * Brigham Young University
 *
 * based on the
 *
 * Original Implementation:
 * Copyright (c) 1995, 1996, 1997 Jim Hugunin, hugunin@mit.edu
 *
 * with inspiration and code from
 * Numarray
 * Space Science Telescope Institute
 * J. Todd Miller
 * Perry Greenfield
 * Rick White
 *
 */
#define NPY_NO_DEPRECATED_API NPY_API_VERSION

#include "Python.h"

#include "npy_config.h"

#define PY_ARRAY_UNIQUE_SYMBOL _mpy_umathmodule_ARRAY_API
#define NO_IMPORT_ARRAY
#define PY_UFUNC_UNIQUE_SYMBOL _mpy_umathmodule_UFUNC_API
#define NO_IMPORT_UFUNC

#include <numpy/npy_3kcompat.h>
#include <numpy/arrayobject.h>
#include <numpy/ufuncobject.h>
#include <numpy/arrayscalars.h>
#include <lowlevel_strided_loops.h>

#define PyMicArray_API_UNIQUE_NAME _mpy_umathmodule_MICARRAY_API
#define PyMicArray_NO_IMPORT
#include <multiarray/common.h>
#include <multiarray/multiarray_api.h>

#define _MICARRAY_UMATHMODULE
#include "mufunc_object.h"
#include "output_creators.h"
#include "reduction.h"

/* Some useful macroes */

#define PyMicArray_TRIVIALLY_ITERABLE(arr) \
            PyArray_TRIVIALLY_ITERABLE((PyArrayObject *)arr)

#define PyMicArray_TRIVIALLY_ITERABLE_PAIR(arr1, arr2) \
            PyArray_TRIVIALLY_ITERABLE_PAIR(\
                    (PyArrayObject *)arr1,(PyArrayObject *)arr2)

#define PyMicArray_TRIVIALLY_ITERABLE_TRIPLE(arr1, arr2, arr3) \
            PyArray_TRIVIALLY_ITERABLE_TRIPLE(\
                (PyArrayObject *)arr1,\
                (PyArrayObject *)arr2,\
                (PyArrayObject *)arr3)

/********** PRINTF DEBUG TRACING **************/
#define NPY_UF_DBG_TRACING 0

#if NPY_UF_DBG_TRACING
#define NPY_UF_DBG_PRINT(s) {printf("%s", s);fflush(stdout);}
#define NPY_UF_DBG_PRINT1(s, p1) {printf((s), (p1));fflush(stdout);}
#define NPY_UF_DBG_PRINT2(s, p1, p2) {printf(s, p1, p2);fflush(stdout);}
#define NPY_UF_DBG_PRINT3(s, p1, p2, p3) {printf(s, p1, p2, p3);fflush(stdout);}
#else
#define NPY_UF_DBG_PRINT(s)
#define NPY_UF_DBG_PRINT1(s, p1)
#define NPY_UF_DBG_PRINT2(s, p1, p2)
#define NPY_UF_DBG_PRINT3(s, p1, p2, p3)
#endif
/**********************************************/


/********************/
#define USE_USE_DEFAULTS 1
/********************/

/* ---------------------------------------------------------------- */

static int
_does_loop_use_arrays(void *data);

static int
_extract_pyvals(PyObject *ref, const char *name, int *bufsize,
                int *errmask, PyObject **errobj);

static int
assign_reduce_identity_zero(PyMicArrayObject *result, void *data);

static int
assign_reduce_identity_minusone(PyMicArrayObject *result, void *data);

static int
assign_reduce_identity_one(PyMicArrayObject *result, void *data);

/*
 * Determine whether all array is on the same device
 * Return 0 on success and -1 when fail
 */
static int
_on_same_device(PyUFuncObject *ufunc, PyMicArrayObject **op)
{
    int nop = ufunc->nin + ufunc->nout;
    if (nop <= 0) {
        return -1;
    }

    int i;
    int device = PyMicArray_DEVICE(op[0]);
    for (i = 1; i < nop; ++i) {
        if (op[i] != NULL && PyMicArray_DEVICE(op[i]) != device) {
            return -1;
        }
    }
    return 0;
}

/*
 * fpstatus is the ufunc_formatted hardware status
 * errmask is the handling mask specified by the user.
 * errobj is a Python object with (string, callable object or None)
 * or NULL
 */

/*
 * 2. for each of the flags
 * determine whether to ignore, warn, raise error, or call Python function.
 * If ignore, do nothing
 * If warn, print a warning and continue
 * If raise return an error
 * If call, call a user-defined function with string
 */

#if USE_USE_DEFAULTS==1
static int PyUFunc_NUM_NODEFAULTS = 0;
#endif

static PyObject *
get_global_ext_obj(void)
{
    PyObject *thedict;
    PyObject *ref = NULL;

#if USE_USE_DEFAULTS==1
    if (PyUFunc_NUM_NODEFAULTS != 0) {
#endif
        thedict = PyThreadState_GetDict();
        if (thedict == NULL) {
            thedict = PyEval_GetBuiltins();
        }
        ref = PyDict_GetItem(thedict, mpy_um_str_pyvals_name);
#if USE_USE_DEFAULTS==1
    }
#endif

    return ref;
}


static int
_get_bufsize_errmask(PyObject * extobj, const char *ufunc_name,
                     int *buffersize, int *errormask)
{
    /* Get the buffersize and errormask */
    if (extobj == NULL) {
        extobj = get_global_ext_obj();
    }
    if (_extract_pyvals(extobj, ufunc_name,
                        buffersize, errormask, NULL) < 0) {
        return -1;
    }

    return 0;
}

static int
_extract_pyvals(PyObject *ref, const char *name, int *bufsize,
                int *errmask, PyObject **errobj)
{
    PyObject *retval;

    /* default errobj case, skips dictionary lookup */
    if (ref == NULL) {
        if (errmask) {
            *errmask = UFUNC_ERR_DEFAULT;
        }
        if (errobj) {
            *errobj = Py_BuildValue("NO", PyBytes_FromString(name), Py_None);
        }
        if (bufsize) {
            *bufsize = NPY_BUFSIZE;
        }
        return 0;
    }

    if (!PyList_Check(ref) || (PyList_GET_SIZE(ref)!=3)) {
        PyErr_Format(PyExc_TypeError,
                "%s must be a length 3 list.", MUFUNC_PYVALS_NAME);
        return -1;
    }

    if (bufsize != NULL) {
        *bufsize = PyInt_AsLong(PyList_GET_ITEM(ref, 0));
        if ((*bufsize == -1) && PyErr_Occurred()) {
            return -1;
        }
        if ((*bufsize < NPY_MIN_BUFSIZE) ||
                (*bufsize > NPY_MAX_BUFSIZE) ||
                (*bufsize % 16 != 0)) {
            PyErr_Format(PyExc_ValueError,
                    "buffer size (%d) is not in range "
                    "(%"NPY_INTP_FMT" - %"NPY_INTP_FMT") or not a multiple of 16",
                    *bufsize, (npy_intp) NPY_MIN_BUFSIZE,
                    (npy_intp) NPY_MAX_BUFSIZE);
            return -1;
        }
    }

    if (errmask != NULL) {
        *errmask = PyInt_AsLong(PyList_GET_ITEM(ref, 1));
        if (*errmask < 0) {
            if (PyErr_Occurred()) {
                return -1;
            }
            PyErr_Format(PyExc_ValueError,
                         "invalid error mask (%d)",
                         *errmask);
            return -1;
        }
    }

    if (errobj != NULL) {
        *errobj = NULL;
        retval = PyList_GET_ITEM(ref, 2);
        if (retval != Py_None && !PyCallable_Check(retval)) {
            PyObject *temp;
            temp = PyObject_GetAttrString(retval, "write");
            if (temp == NULL || !PyCallable_Check(temp)) {
                PyErr_SetString(PyExc_TypeError,
                                "python object must be callable or have " \
                                "a callable write method");
                Py_XDECREF(temp);
                return -1;
            }
            Py_DECREF(temp);
        }

        *errobj = Py_BuildValue("NO", PyBytes_FromString(name), retval);
        if (*errobj == NULL) {
            return -1;
        }
    }
    return 0;
}

/* Return the position of next non-white-space char in the string */
static int
_next_non_white_space(const char* str, int offset)
{
    int ret = offset;
    while (str[ret] == ' ' || str[ret] == '\t') {
        ret++;
    }
    return ret;
}

static int
_is_alpha_underscore(char ch)
{
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '_';
}

static int
_is_alnum_underscore(char ch)
{
    return _is_alpha_underscore(ch) || (ch >= '0' && ch <= '9');
}

/*
 * Return the ending position of a variable name
 */
static int
_get_end_of_name(const char* str, int offset)
{
    int ret = offset;
    while (_is_alnum_underscore(str[ret])) {
        ret++;
    }
    return ret;
}

/*
 * Returns 1 if the dimension names pointed by s1 and s2 are the same,
 * otherwise returns 0.
 */
static int
_is_same_name(const char* s1, const char* s2)
{
    while (_is_alnum_underscore(*s1) && _is_alnum_underscore(*s2)) {
        if (*s1 != *s2) {
            return 0;
        }
        s1++;
        s2++;
    }
    return !_is_alnum_underscore(*s1) && !_is_alnum_underscore(*s2);
}


/*
 * Checks if 'obj' is a valid output array for a ufunc, i.e. it is
 * either None or a writeable array, increments its reference count
 * and stores a pointer to it in 'store'. Returns 0 on success, sets
 * an exception and returns -1 on failure.
 */
static int
_set_out_array(PyObject *obj, PyMicArrayObject **store)
{
    if (obj == Py_None) {
        /* Translate None to NULL */
        return 0;
    }
    if PyMicArray_Check(obj) {
        /* If it's an array, store it */
        if (PyMicArray_FailUnlessWriteable((PyMicArrayObject *)obj,
                                        "output array") < 0) {
            return -1;
        }
        Py_INCREF(obj);
        *store = (PyMicArrayObject *)obj;

        return 0;
    }

    PyErr_SetString(PyExc_TypeError, "return arrays must be of ArrayType");
    return -1;
}

/********* GENERIC UFUNC USING ITERATOR *********/

/*
 * Parses the positional and keyword arguments for a generic ufunc call.
 *
 * Note that if an error is returned, the caller must free the
 * non-zero references in out_op.  This
 * function does not do its own clean-up.
 */
static int
get_ufunc_arguments(PyUFuncObject *ufunc,
                    PyObject *args, PyObject *kwds,
                    PyMicArrayObject **out_op,
                    NPY_ORDER *out_order,
                    NPY_CASTING *out_casting,
                    PyObject **out_extobj,
                    PyObject **out_typetup,
                    int *out_subok,
                    PyMicArrayObject **out_wheremask)
{
    int i, nargs;
    int nin = ufunc->nin;
    int nout = ufunc->nout;
    PyObject *obj, *context;
    PyObject *str_key_obj = NULL;
    const char *ufunc_name;
    int type_num;

    int any_flexible = 0, any_object = 0, any_flexible_userloops = 0;
    int has_sig = 0;

    ufunc_name = ufunc->name ? ufunc->name : "<unnamed ufunc>";

    *out_extobj = NULL;
    *out_typetup = NULL;
    if (out_wheremask != NULL) {
        *out_wheremask = NULL;
    }

    /* Check number of arguments */
    nargs = PyTuple_Size(args);
    if ((nargs < nin) || (nargs > ufunc->nargs)) {
        PyErr_SetString(PyExc_ValueError, "invalid number of arguments");
        return -1;
    }

    /* Get input arguments */
    for (i = 0; i < nin; ++i) {
        obj = PyTuple_GET_ITEM(args, i);

        if (PyMicArray_Check(obj)) {
            PyMicArrayObject *obj_a = (PyMicArrayObject *)obj;
            out_op[i] = (PyMicArrayObject *)PyMicArray_FromArray((PyArrayObject *)obj_a,
                            NULL, PyMicArray_DEVICE(obj_a), 0);
        }
        else if (PyArray_IsScalar(obj, Generic)) {
            /*
             * TODO: Convert scalar to PyMicArray
             */
        }

        if (out_op[i] == NULL) {
            return -1;
        }

        type_num = PyMicArray_DESCR(out_op[i])->type_num;
        if (!any_flexible &&
                PyTypeNum_ISFLEXIBLE(type_num)) {
            any_flexible = 1;
        }
        if (!any_object &&
                PyTypeNum_ISOBJECT(type_num)) {
            any_object = 1;
        }

        /*
         * If any operand is a flexible dtype, check to see if any
         * struct dtype ufuncs are registered. A ufunc has been registered
         * for a struct dtype if ufunc's arg_dtypes array is not NULL.
         */
        if (PyTypeNum_ISFLEXIBLE(type_num) &&
                    !any_flexible_userloops &&
                    ufunc->userloops != NULL) {
            PyUFunc_Loop1d *funcdata;
            PyObject *key, *obj;
            key = PyInt_FromLong(type_num);
            if (key == NULL) {
                continue;
            }
            obj = PyDict_GetItem(ufunc->userloops, key);
            Py_DECREF(key);
            if (obj == NULL) {
                continue;
            }
            funcdata = (PyUFunc_Loop1d *)NpyCapsule_AsVoidPtr(obj);
            while (funcdata != NULL) {
                if (funcdata->arg_dtypes != NULL) {
                    any_flexible_userloops = 1;
                    break;
                }
                funcdata = funcdata->next;
            }
        }
    }

    if (any_flexible && !any_flexible_userloops && !any_object) {
        /* Traditionally, we return -2 here (meaning "NotImplemented") anytime
         * we hit the above condition.
         *
         * This condition basically means "we are doomed", b/c the "flexible"
         * dtypes -- strings and void -- cannot have their own ufunc loops
         * registered (except via the special "flexible userloops" mechanism),
         * and they can't be cast to anything except object (and we only cast
         * to object if any_object is true). So really we should do nothing
         * here and continue and let the proper error be raised. But, we can't
         * quite yet, b/c of backcompat.
         *
         * Most of the time, this NotImplemented either got returned directly
         * to the user (who can't do anything useful with it), or got passed
         * back out of a special function like __mul__. And fortunately, for
         * almost all special functions, the end result of this was a
         * TypeError. Which is also what we get if we just continue without
         * this special case, so this special case is unnecessary.
         *
         * The only thing that actually depended on the NotImplemented is
         * array_richcompare, which did two things with it. First, it needed
         * to see this NotImplemented in order to implement the special-case
         * comparisons for
         *
         *    string < <= == != >= > string
         *    void == != void
         *
         * Now it checks for those cases first, before trying to call the
         * ufunc, so that's no problem. What it doesn't handle, though, is
         * cases like
         *
         *    float < string
         *
         * or
         *
         *    float == void
         *
         * For those, it just let the NotImplemented bubble out, and accepted
         * Python's default handling. And unfortunately, for comparisons,
         * Python's default is *not* to raise an error. Instead, it returns
         * something that depends on the operator:
         *
         *    ==         return False
         *    !=         return True
         *    < <= >= >  Python 2: use "fallback" (= weird and broken) ordering
         *               Python 3: raise TypeError (hallelujah)
         *
         * In most cases this is straightforwardly broken, because comparison
         * of two arrays should always return an array, and here we end up
         * returning a scalar. However, there is an exception: if we are
         * comparing two scalars for equality, then it actually is correct to
         * return a scalar bool instead of raising an error. If we just
         * removed this special check entirely, then "np.float64(1) == 'foo'"
         * would raise an error instead of returning False, which is genuinely
         * wrong.
         *
         * The proper end goal here is:
         *   1) == and != should be implemented in a proper vectorized way for
         *      all types. The short-term hack for this is just to add a
         *      special case to PyUFunc_DefaultLegacyInnerLoopSelector where
         *      if it can't find a comparison loop for the given types, and
         *      the ufunc is np.equal or np.not_equal, then it returns a loop
         *      that just fills the output array with False (resp. True). Then
         *      array_richcompare could trust that whenever its special cases
         *      don't apply, simply calling the ufunc will do the right thing,
         *      even without this special check.
         *   2) < <= >= > should raise an error if no comparison function can
         *      be found. array_richcompare already handles all string <>
         *      string cases, and void dtypes don't have ordering, so again
         *      this would mean that array_richcompare could simply call the
         *      ufunc and it would do the right thing (i.e., raise an error),
         *      again without needing this special check.
         *
         * So this means that for the transition period, our goal is:
         *   == and != on scalars should simply return NotImplemented like
         *     they always did, since everything ends up working out correctly
         *     in this case only
         *   == and != on arrays should issue a FutureWarning and then return
         *     NotImplemented
         *   < <= >= > on all flexible dtypes on py2 should raise a
         *     DeprecationWarning, and then return NotImplemented. On py3 we
         *     skip the warning, though, b/c it would just be immediately be
         *     followed by an exception anyway.
         *
         * And for all other operations, we let things continue as normal.
         */
        /* strcmp() is a hack but I think we can get away with it for this
         * temporary measure.
         */
        if (!strcmp(ufunc_name, "equal") ||
                !strcmp(ufunc_name, "not_equal")) {
            /* Warn on non-scalar, return NotImplemented regardless */
            assert(nin == 2);
            if (PyMicArray_NDIM(out_op[0]) != 0 ||
                    PyMicArray_NDIM(out_op[1]) != 0) {
                if (DEPRECATE_FUTUREWARNING(
                        "elementwise comparison failed; returning scalar "
                        "instead, but in the future will perform elementwise "
                        "comparison") < 0) {
                    return -1;
                }
            }
            return -2;
        }
        else if (!strcmp(ufunc_name, "less") ||
                 !strcmp(ufunc_name, "less_equal") ||
                 !strcmp(ufunc_name, "greater") ||
                 !strcmp(ufunc_name, "greater_equal")) {
#if !defined(NPY_PY3K)
            if (DEPRECATE("unorderable dtypes; returning scalar but in "
                          "the future this will be an error") < 0) {
                return -1;
            }
#endif
            return -2;
        }
    }

    /* Get positional output arguments */
    for (i = nin; i < nargs; ++i) {
        obj = PyTuple_GET_ITEM(args, i);
        if (_set_out_array(obj, out_op + i) < 0) {
            return -1;
        }
    }

    /*
     * Get keyword output and other arguments.
     * Raise an error if anything else is present in the
     * keyword dictionary.
     */
    if (kwds != NULL) {
        PyObject *key, *value;
        Py_ssize_t pos = 0;
        while (PyDict_Next(kwds, &pos, &key, &value)) {
            Py_ssize_t length = 0;
            char *str = NULL;
            int bad_arg = 1;

#if defined(NPY_PY3K)
            Py_XDECREF(str_key_obj);
            str_key_obj = PyUnicode_AsASCIIString(key);
            if (str_key_obj != NULL) {
                key = str_key_obj;
            }
#endif

            if (PyBytes_AsStringAndSize(key, &str, &length) < 0) {
                PyErr_Clear();
                PyErr_SetString(PyExc_TypeError, "invalid keyword argument");
                goto fail;
            }

            switch (str[0]) {
                case 'c':
                    /* Provides a policy for allowed casting */
                    if (strcmp(str, "casting") == 0) {
                        if (!PyArray_CastingConverter(value, out_casting)) {
                            goto fail;
                        }
                        bad_arg = 0;
                    }
                    break;
                case 'd':
                    /* Another way to specify 'sig' */
                    if (strcmp(str, "dtype") == 0) {
                        /* Allow this parameter to be None */
                        PyArray_Descr *dtype;
                        if (!PyArray_DescrConverter2(value, &dtype)) {
                            goto fail;
                        }
                        if (dtype != NULL) {
                            if (*out_typetup != NULL) {
                                PyErr_SetString(PyExc_RuntimeError,
                                    "cannot specify both 'sig' and 'dtype'");
                                goto fail;
                            }
                            *out_typetup = Py_BuildValue("(N)", dtype);
                        }
                        bad_arg = 0;
                    }
                    break;
                case 'e':
                    /*
                     * Overrides the global parameters buffer size,
                     * error mask, and error object
                     */
                    if (strcmp(str, "extobj") == 0) {
                        *out_extobj = value;
                        bad_arg = 0;
                    }
                    break;
                case 'o':
                    /*
                     * Output arrays may be specified as a keyword argument,
                     * either as a single array or None for single output
                     * ufuncs, or as a tuple of arrays and Nones.
                     */
                    if (strcmp(str, "out") == 0) {
                        if (nargs > nin) {
                            PyErr_SetString(PyExc_ValueError,
                                    "cannot specify 'out' as both a "
                                    "positional and keyword argument");
                            goto fail;
                        }
                        if (PyTuple_Check(value)) {
                            if (PyTuple_GET_SIZE(value) != nout) {
                                PyErr_SetString(PyExc_ValueError,
                                        "The 'out' tuple must have exactly "
                                        "one entry per ufunc output");
                                goto fail;
                            }
                            /* 'out' must be a tuple of arrays and Nones */
                            for(i = 0; i < nout; ++i) {
                                PyObject *val = PyTuple_GET_ITEM(value, i);
                                if (_set_out_array(val, out_op+nin+i) < 0) {
                                    goto fail;
                                }
                            }
                        }
                        else if (nout == 1) {
                            /* Can be an array if it only has one output */
                            if (_set_out_array(value, out_op + nin) < 0) {
                                goto fail;
                            }
                        }
                        else {
                            PyErr_SetString(PyExc_TypeError,
                                    nout > 1 ? "'out' must be a tuple "
                                    "of arrays" :
                                    "'out' must be an array or a "
                                    "tuple of a single array");
                            goto fail;
                        }
                        bad_arg = 0;
                    }
                    /* Allows the default output layout to be overridden */
                    else if (strcmp(str, "order") == 0) {
                        if (!PyArray_OrderConverter(value, out_order)) {
                            goto fail;
                        }
                        bad_arg = 0;
                    }
                    break;
                case 's':
                    /* TODO: remove??? */
                    /* Allows a specific function inner loop to be selected */
                    if (strcmp(str, "sig") == 0 ||
                            strcmp(str, "signature") == 0) {
                        if (has_sig == 1) {
                            PyErr_SetString(PyExc_ValueError,
                                "cannot specify both 'sig' and 'signature'");
                            goto fail;
                        }
                        if (*out_typetup != NULL) {
                            PyErr_SetString(PyExc_RuntimeError,
                                    "cannot specify both 'sig' and 'dtype'");
                            goto fail;
                        }
                        *out_typetup = value;
                        Py_INCREF(value);
                        bad_arg = 0;
                        has_sig = 1;
                    }
                    else if (strcmp(str, "subok") == 0) {
                        if (!PyBool_Check(value)) {
                            PyErr_SetString(PyExc_TypeError,
                                        "'subok' must be a boolean");
                            goto fail;
                        }
                        *out_subok = (value == Py_True);
                        bad_arg = 0;
                    }
                    break;
                case 'w':
                    /*
                     * Provides a boolean array 'where=' mask if
                     * out_wheremask is supplied.
                     */
                    if (out_wheremask != NULL && strcmp(str, "where") == 0) {
                        if (PyMicArray_Check(value) && PyMicArray_ISBOOL(value)) {
                            *out_wheremask = (PyMicArrayObject *)value;
                        }
                        else {
                            /* TODO: convert to mic array of bool */
                            PyArray_Descr *dtype;
                            dtype = PyArray_DescrFromType(NPY_BOOL);
                            if (dtype == NULL) {
                                goto fail;
                            }
                            //*out_wheremask = (PyMicArrayObject *)PyMicArray_FromAny(
                            //                                    value, dtype,
                            //                                    0, 0, 0, NULL);
                        }
                        if (*out_wheremask == NULL) {
                            goto fail;
                        }
                        bad_arg = 0;
                    }
                    break;
            }

            if (bad_arg) {
                char *format = "'%s' is an invalid keyword to ufunc '%s'";
                PyErr_Format(PyExc_TypeError, format, str, ufunc_name);
                goto fail;
            }
        }
    }
    Py_XDECREF(str_key_obj);

    return 0;

fail:
    Py_XDECREF(str_key_obj);
    Py_XDECREF(*out_extobj);
    *out_extobj = NULL;
    Py_XDECREF(*out_typetup);
    *out_typetup = NULL;
    if (out_wheremask != NULL) {
        Py_XDECREF(*out_wheremask);
        *out_wheremask = NULL;
    }
    return -1;
}

/*
 * This checks whether a trivial loop is ok,
 * making copies of scalar and one dimensional operands if that will
 * help.
 *
 * Returns 1 if a trivial loop is ok, 0 if it is not, and
 * -1 if there is an error.
 */
static int
check_for_trivial_loop(PyUFuncObject *ufunc,
                        PyMicArrayObject **op,
                        PyArray_Descr **dtype,
                        npy_intp buffersize)
{
    npy_intp i, nin = ufunc->nin, nop = nin + ufunc->nout;

    for (i = 0; i < nop; ++i) {
        /*
         * If the dtype doesn't match, or the array isn't aligned,
         * indicate that the trivial loop can't be done.
         */
        if (op[i] != NULL &&
                (!PyMicArray_ISALIGNED(op[i]) ||
                !PyArray_EquivTypes(dtype[i], PyMicArray_DESCR(op[i]))
                                        )) {
            /*
             * If op[j] is a scalar or small one dimensional
             * array input, make a copy to keep the opportunity
             * for a trivial loop.
             */
            if (i < nin && (PyMicArray_NDIM(op[i]) == 0 ||
                    (PyMicArray_NDIM(op[i]) == 1 &&
                     PyMicArray_DIM(op[i],0) <= buffersize))) {
                PyMicArrayObject *tmp;
                Py_INCREF(dtype[i]);
                tmp = (PyMicArrayObject *)
                            PyMicArray_FromArray((PyArrayObject *)op[i], dtype[i],
                                    PyMicArray_DEVICE(op[i]), 0);
                if (tmp == NULL) {
                    Py_DECREF(dtype[i]);
                    return -1;
                }
                Py_DECREF(op[i]);
                op[i] = tmp;
            }
            else {
                return 0;
            }
        }
    }

    return 1;
}

static void
trivial_two_operand_loop(PyMicArrayObject **op,
                    PyUFuncGenericFunction innerloop,
                    void *innerloopdata)
{
    char *data[2];
    npy_intp count[2], stride[2];
    int needs_api, device;
    NPY_BEGIN_THREADS_DEF;

    needs_api = PyDataType_REFCHK(PyMicArray_DESCR(op[0])) ||
                PyDataType_REFCHK(PyMicArray_DESCR(op[1]));


    device = PyMicArray_DEVICE(op[0]);
    PyArray_PREPARE_TRIVIAL_PAIR_ITERATION((PyArrayObject *)op[0],
                                            (PyArrayObject *)op[1],
                                            count[0],
                                            data[0], data[1],
                                            stride[0], stride[1]);
    count[1] = count[0];
    NPY_UF_DBG_PRINT1("two operand loop count %d\n", (int)count[0]);

    if (!needs_api) {
        NPY_BEGIN_THREADS_THRESHOLDED(count[0]);
    }


#pragma omp target device(device) \
    map(to: innerloop, innerloopdata, \
            count[0:2], stride[0:2])
    innerloop(NULL, count, stride, innerloopdata);

    NPY_END_THREADS;
}

static void
trivial_three_operand_loop(PyMicArrayObject **op,
                    PyUFuncGenericFunction innerloop,
                    void *innerloopdata)
{
    char *data[3];
    npy_intp count[3], stride[3];
    int needs_api, device;
    NPY_BEGIN_THREADS_DEF;

    needs_api = PyDataType_REFCHK(PyMicArray_DESCR(op[0])) ||
                PyDataType_REFCHK(PyMicArray_DESCR(op[1])) ||
                PyDataType_REFCHK(PyMicArray_DESCR(op[2]));

    device = PyMicArray_DEVICE(op[0]);
    PyArray_PREPARE_TRIVIAL_TRIPLE_ITERATION((PyArrayObject *)op[0],
                                            (PyArrayObject *)op[1],
                                            (PyArrayObject *)op[2],
                                            count[0],
                                            data[0], data[1], data[2],
                                            stride[0], stride[1], stride[2]);
    count[1] = count[0];
    count[2] = count[0];
    NPY_UF_DBG_PRINT1("three operand loop count %d\n", (int)count[0]);

    if (!needs_api) {
        NPY_BEGIN_THREADS_THRESHOLDED(count[0]);
    }

#pragma omp target device(device) \
    map(to: innerloop, innerloopdata, \
            data[0:3], count[0:3], stride[0:3])
    innerloop(data, count, stride, innerloopdata);

    NPY_END_THREADS;
}


static int
iterator_loop(PyUFuncObject *ufunc,
                    PyMicArrayObject **op,
                    PyArray_Descr **dtype,
                    NPY_ORDER order,
                    npy_intp buffersize,
                    PyObject **arr_prep,
                    PyObject *arr_prep_args,
                    PyUFuncGenericFunction innerloop,
                    void *innerloopdata)
{
    npy_intp i, nin = ufunc->nin, nout = ufunc->nout;
    npy_intp nop = nin + nout;
    npy_uint32 op_flags[NPY_MAXARGS];
    NpyIter *iter;
    char *baseptrs[NPY_MAXARGS];

    NpyIter_IterNextFunc *iternext;
    char **dataptr;
    npy_intp *stride;
    npy_intp *size;
    int device;

    //PyArrayObject **op_it;
    PyArrayObject *op_npy[nop];
    PyMicArrayObject *op_new[nout];
    int new_count = 0;
    npy_uint32 iter_flags;

    device = PyMUFunc_GetCommonDevice(nop, op);

    NPY_BEGIN_THREADS_DEF;

    /* Set up the flags */
    for (i = 0; i < nin; ++i) {
        op_flags[i] = NPY_ITER_READONLY |
                      NPY_ITER_ALIGNED;
        /*
         * If READWRITE flag has been set for this operand,
         * then clear default READONLY flag
         */
        op_flags[i] |= ufunc->op_flags[i];
        if (op_flags[i] & (NPY_ITER_READWRITE | NPY_ITER_WRITEONLY)) {
            op_flags[i] &= ~NPY_ITER_READONLY;
        }
    }
    for (i = nin; i < nop; ++i) {
        op_flags[i] = NPY_ITER_WRITEONLY |
                      NPY_ITER_ALIGNED |
                      NPY_ITER_NO_BROADCAST |
                      NPY_ITER_NO_SUBTYPE;
    }

    iter_flags = ufunc->iter_flags |
                 NPY_ITER_EXTERNAL_LOOP |
                 NPY_ITER_REFS_OK |
                 NPY_ITER_ZEROSIZE_OK;

    /* Allocate output result */
    for (i = nin; i < nop; ++i) {
        PyMicArrayObject *tmp;
        if (op[i] == NULL) {
            tmp = (PyMicArrayObject *)
                PyMUFunc_CreateArrayBroadcast(nin, op, dtype[i]);

            if (tmp == NULL)
                goto fail;
            op[i] = tmp;
            op_new[new_count++] = tmp;
        }
    }

    /* Copy to temporary PyArrayObject * array */
    for (i = 0; i < nop; ++i) {
        op_npy[i] = (PyArrayObject *) op[i];
    }


    /*
     * Allocate the iterator.  Because the types of the inputs
     * were already checked, we use the casting rule 'unsafe' which
     * is faster to calculate.
     */
    iter = NpyIter_MultiNew(nop, op_npy, iter_flags, order,
                            NPY_UNSAFE_CASTING, op_flags, dtype);
    if (iter == NULL) {
        goto fail;
    }

    /* Copy any allocated outputs */
    /*
    op_it = NpyIter_GetOperandArray(iter);
    for (i = nin; i < nop; ++i) {
        if (op[i] == NULL) {
            op[i] = op_it[i];
            Py_INCREF(op[i]);
        }
    }
    */

    /* Call the __array_prepare__ functions where necessary */
    /*for (i = 0; i < nout; ++i) {
        if (prepare_ufunc_output(ufunc, &op[nin+i],
                            arr_prep[i], arr_prep_args, i) < 0) {
            NpyIter_Deallocate(iter);
            return -1;
        }
    }
    */

    /* Only do the loop if the iteration size is non-zero */
    if (NpyIter_GetIterSize(iter) != 0) {

        /* Reset the iterator with the base pointers from the wrapped outputs */
        for (i = 0; i < nop; ++i) {
            baseptrs[i] = PyMicArray_BYTES(op[i]);
        }
        if (NpyIter_ResetBasePointers(iter, baseptrs, NULL) != NPY_SUCCEED) {
            NpyIter_Deallocate(iter);
            goto fail;
        }

        /* Get the variables needed for the loop */
        iternext = NpyIter_GetIterNext(iter, NULL);
        if (iternext == NULL) {
            NpyIter_Deallocate(iter);
            goto fail;
        }
        dataptr = NpyIter_GetDataPtrArray(iter);
        stride = NpyIter_GetInnerStrideArray(iter);
        size = NpyIter_GetInnerLoopSizePtr(iter);

        NPY_BEGIN_THREADS_NDITER(iter);

        /* Execute the loop */
        do {
            NPY_UF_DBG_PRINT1("iterator loop count %d\n", (int)*count_ptr);
#pragma omp target device(device) \
            map(to: innerloop, innerloopdata, \
                    size[0:1], \
                    stride[0:nop])
            innerloop(NULL, size, stride, innerloopdata);
        } while (iternext(iter));

        NPY_END_THREADS;
    }

    NpyIter_Deallocate(iter);
    return 0;

fail:
    /* Clean up allocated array */
    for (i = 0; i < new_count; ++i) {
        Py_DECREF(op_new[i]);
    }
    return -1;
}

/*
 * trivial_loop_ok - 1 if no alignment, data conversion, etc required
 * nin             - number of inputs
 * nout            - number of outputs
 * op              - the operands (nin + nout of them)
 * order           - the loop execution order/output memory order
 * buffersize      - how big of a buffer to use
 * arr_prep        - the __array_prepare__ functions for the outputs
 * innerloop       - the inner loop function
 * innerloopdata   - data to pass to the inner loop
 */
static int
execute_legacy_ufunc_loop(PyUFuncObject *ufunc,
                    int trivial_loop_ok,
                    PyMicArrayObject **op,
                    PyArray_Descr **dtypes,
                    NPY_ORDER order,
                    npy_intp buffersize,
                    PyObject **arr_prep,
                    PyObject *arr_prep_args)
{
    npy_intp nin = ufunc->nin, nout = ufunc->nout;
    PyUFuncGenericFunction innerloop;
    void *innerloopdata;
    int needs_api = 0;

    if (ufunc->legacy_inner_loop_selector(ufunc, dtypes,
                    &innerloop, &innerloopdata, &needs_api) < 0) {
        return -1;
    }
    /* If the loop wants the arrays, provide them. */
    innerloopdata = (void*)op;

    /* First check for the trivial cases that don't need an iterator */
    if (trivial_loop_ok) {
        if (nin == 1 && nout == 1) {
            if (op[1] == NULL &&
                        (order == NPY_ANYORDER || order == NPY_KEEPORDER) &&
                        PyMicArray_TRIVIALLY_ITERABLE(op[0])) {
                Py_INCREF(dtypes[1]);
                op[1] = (PyMicArrayObject *)PyMicArray_NewFromDescr(
                             PyMicArray_DEVICE(op[0]),
                             &PyMicArray_Type,
                             dtypes[1],
                             PyMicArray_NDIM(op[0]),
                             PyMicArray_DIMS(op[0]),
                             NULL, NULL,
                             PyMicArray_ISFORTRAN(op[0]) ?
                                            NPY_ARRAY_F_CONTIGUOUS : 0,
                             NULL);
                if (op[1] == NULL) {
                    return -1;
                }

                NPY_UF_DBG_PRINT("trivial 1 input with allocated output\n");
                trivial_two_operand_loop(op, innerloop, innerloopdata);

                return 0;
            }
            else if (op[1] != NULL &&
                        PyMicArray_NDIM(op[1]) >= PyMicArray_NDIM(op[0]) &&
                        PyMicArray_TRIVIALLY_ITERABLE_PAIR(op[0], op[1])) {

                NPY_UF_DBG_PRINT("trivial 1 input\n");
                trivial_two_operand_loop(op, innerloop, innerloopdata);

                return 0;
            }
        }
        else if (nin == 2 && nout == 1) {
            if (op[2] == NULL &&
                        (order == NPY_ANYORDER || order == NPY_KEEPORDER) &&
                        PyMicArray_TRIVIALLY_ITERABLE_PAIR(op[0], op[1])) {
                PyMicArrayObject *tmp;
                /*
                 * Have to choose the input with more dimensions to clone, as
                 * one of them could be a scalar.
                 */
                if (PyMicArray_NDIM(op[0]) >= PyMicArray_NDIM(op[1])) {
                    tmp = op[0];
                }
                else {
                    tmp = op[1];
                }
                Py_INCREF(dtypes[2]);
                op[2] = (PyMicArrayObject *)PyMicArray_NewFromDescr(
                                 PyMicArray_DEVICE(tmp),
                                 &PyMicArray_Type,
                                 dtypes[2],
                                 PyMicArray_NDIM(tmp),
                                 PyMicArray_DIMS(tmp),
                                 NULL, NULL,
                                 PyMicArray_ISFORTRAN(tmp) ?
                                                NPY_ARRAY_F_CONTIGUOUS : 0,
                                 NULL);
                if (op[2] == NULL) {
                    return -1;
                }

                NPY_UF_DBG_PRINT("trivial 2 input with allocated output\n");
                trivial_three_operand_loop(op, innerloop, innerloopdata);

                return 0;
            }
            else if (op[2] != NULL &&
                    PyMicArray_NDIM(op[2]) >= PyMicArray_NDIM(op[0]) &&
                    PyMicArray_NDIM(op[2]) >= PyMicArray_NDIM(op[1]) &&
                    PyMicArray_TRIVIALLY_ITERABLE_TRIPLE(op[0], op[1], op[2])) {

                NPY_UF_DBG_PRINT("trivial 2 input\n");
                trivial_three_operand_loop(op, innerloop, innerloopdata);

                return 0;
            }
        }
    }

    /*
     * If no trivial loop matched, an iterator is required to
     * resolve broadcasting, etc
     */

    NPY_UF_DBG_PRINT("iterator loop\n");
    if (iterator_loop(ufunc, op, dtypes, order,
                    buffersize, arr_prep, arr_prep_args,
                    innerloop, innerloopdata) < 0) {
        return -1;
    }

    return 0;
}

/*
 * nin             - number of inputs
 * nout            - number of outputs
 * wheremask       - if not NULL, the 'where=' parameter to the ufunc.
 * op              - the operands (nin + nout of them)
 * order           - the loop execution order/output memory order
 * buffersize      - how big of a buffer to use
 * arr_prep        - the __array_prepare__ functions for the outputs
 * innerloop       - the inner loop function
 * innerloopdata   - data to pass to the inner loop
 */
static int
execute_fancy_ufunc_loop(PyUFuncObject *ufunc,
                    PyMicArrayObject *wheremask,
                    PyMicArrayObject **op,
                    PyArray_Descr **dtypes,
                    NPY_ORDER order,
                    npy_intp buffersize,
                    PyObject **arr_prep,
                    PyObject *arr_prep_args)
{
    int i, nin = ufunc->nin, nout = ufunc->nout;
    int nop = nin + nout;
    int nop_real;
    npy_uint32 op_flags[NPY_MAXARGS];
    NpyIter *iter;
    int device;
    npy_intp default_op_in_flags = 0, default_op_out_flags = 0;

    NpyIter_IterNextFunc *iternext;
    char **dataptr;
    npy_intp *strides;
    npy_intp *countptr;


    PyArrayObject *op_npy[NPY_MAXARGS];
    PyMicArrayObject *op_new[nop];
    int count_new = 0;
    npy_uint32 iter_flags;

    device = PyMUFunc_GetCommonDevice(nin, op);

    if (wheremask != NULL) {
        if (nop + 1 > NPY_MAXARGS) {
            PyErr_SetString(PyExc_ValueError,
                    "Too many operands when including where= parameter");
            return -1;
        }
        op[nop] = wheremask;
        dtypes[nop] = NULL;
        default_op_out_flags |= NPY_ITER_WRITEMASKED;
    }

    /* Set up the flags */
    for (i = 0; i < nin; ++i) {
        op_flags[i] = default_op_in_flags |
                      NPY_ITER_READONLY |
                      NPY_ITER_ALIGNED;
        /*
         * If READWRITE flag has been set for this operand,
         * then clear default READONLY flag
         */
        op_flags[i] |= ufunc->op_flags[i];
        if (op_flags[i] & (NPY_ITER_READWRITE | NPY_ITER_WRITEONLY)) {
            op_flags[i] &= ~NPY_ITER_READONLY;
        }
    }
    for (i = nin; i < nop; ++i) {
        op_flags[i] = default_op_out_flags |
                      NPY_ITER_WRITEONLY |
                      NPY_ITER_ALIGNED |
                      NPY_ITER_NO_BROADCAST |
                      NPY_ITER_NO_SUBTYPE;
    }
    if (wheremask != NULL) {
        op_flags[nop] = NPY_ITER_READONLY | NPY_ITER_ARRAYMASK;
    }

    NPY_UF_DBG_PRINT("Making iterator\n");

    iter_flags = ufunc->iter_flags |
                 NPY_ITER_EXTERNAL_LOOP |
                 NPY_ITER_REFS_OK |
                 NPY_ITER_ZEROSIZE_OK;

    /* Allocate output array */
    for (i = nin; i < nop; ++i) {
        PyMicArrayObject *tmp;
        if (op[i] == NULL) {
            tmp = PyMUFunc_CreateArrayBroadcast(nin, op, dtypes[i]);

            if (tmp == NULL) {
                goto fail;
            }

            op[i] = tmp;
            op_new[count_new++] = tmp;
        }
    }

    /* Copy two array of PyArrayObject * */
    for (i = 0; i < nop; ++i) {
        op_npy[i] = (PyArrayObject *) op[i];
    }
    nop_real = nop + ((wheremask != NULL) ? 1 : 0);

    /*
     * Allocate the iterator.  Because the types of the inputs
     * were already checked, we use the casting rule 'unsafe' which
     * is faster to calculate.
     */
    iter = NpyIter_MultiNew(nop_real, op_npy,
                        iter_flags,
                        order, NPY_UNSAFE_CASTING,
                        op_flags, dtypes);
    if (iter == NULL) {
        goto fail;
    }

    NPY_UF_DBG_PRINT("Made iterator\n");

    /* Call the __array_prepare__ functions where necessary */
    /*
    for (i = 0; i < nout; ++i) {
        if (prepare_ufunc_output(ufunc, &op[nin+i],
                            arr_prep[i], arr_prep_args, i) < 0) {
            NpyIter_Deallocate(iter);
            return -1;
        }
    }
    */

    /* Only do the loop if the iteration size is non-zero */
    if (NpyIter_GetIterSize(iter) != 0) {
        PyUFunc_MaskedStridedInnerLoopFunc *innerloop;
        NpyAuxData *innerloopdata;
        npy_intp fixed_strides[2*NPY_MAXARGS];
        PyArray_Descr **iter_dtypes;
        NPY_BEGIN_THREADS_DEF;

        /* Validate that the prepare_ufunc_output didn't mess with pointers */
        /*
        for (i = nin; i < nop; ++i) {
            if (PyArray_BYTES(op[i]) != PyArray_BYTES(op_it[i])) {
                PyErr_SetString(PyExc_ValueError,
                        "The __array_prepare__ functions modified the data "
                        "pointer addresses in an invalid fashion");
                NpyIter_Deallocate(iter);
                return -1;
            }
        }
        */

        /*
         * Get the inner loop, with the possibility of specialization
         * based on the fixed strides.
         */
        NpyIter_GetInnerFixedStrideArray(iter, fixed_strides);
        iter_dtypes = NpyIter_GetDescrArray(iter);
        if (ufunc->masked_inner_loop_selector(ufunc, dtypes,
                        wheremask != NULL ? iter_dtypes[nop]
                                          : iter_dtypes[nop + nin],
                        fixed_strides,
                        wheremask != NULL ? fixed_strides[nop]
                                          : fixed_strides[nop + nin],
                        &innerloop, &innerloopdata, 0) < 0) {
            NpyIter_Deallocate(iter);
            return -1;
        }

        /* Get the variables needed for the loop */
        iternext = NpyIter_GetIterNext(iter, NULL);
        if (iternext == NULL) {
            NpyIter_Deallocate(iter);
            return -1;
        }
        dataptr = NpyIter_GetDataPtrArray(iter);
        strides = NpyIter_GetInnerStrideArray(iter);
        countptr = NpyIter_GetInnerLoopSizePtr(iter);

        NPY_BEGIN_THREADS_NDITER(iter);

        NPY_UF_DBG_PRINT("Actual inner loop:\n");
        /* Execute the loop */
        do {
            NPY_UF_DBG_PRINT1("iterator loop count %d\n", (int)*countptr);
            npy_intp count = *countptr;
#pragma omp target device(device) \
            map(to: innerloop, count, innerloopdata,\
                    strides[0:nop_real])
            innerloop(NULL, strides,
                        NULL, strides[nop],
                        count, innerloopdata);
        } while (iternext(iter));

        NPY_END_THREADS;

        NPY_AUXDATA_FREE(innerloopdata);
    }

    NpyIter_Deallocate(iter);
    return 0;

fail:
    for (i = 0; i < count_new; ++i) {
        Py_DECREF(op_new[i]);
    }
    return -1;
}

static PyObject *
make_arr_prep_args(npy_intp nin, PyObject *args, PyObject *kwds)
{
    PyObject *out = kwds ? PyDict_GetItem(kwds, mpy_um_str_out) : NULL;
    PyObject *arr_prep_args;

    if (out == NULL) {
        Py_INCREF(args);
        return args;
    }
    else {
        npy_intp i, nargs = PyTuple_GET_SIZE(args), n;
        n = nargs;
        if (n < nin + 1) {
            n = nin + 1;
        }
        arr_prep_args = PyTuple_New(n);
        if (arr_prep_args == NULL) {
            return NULL;
        }
        /* Copy the tuple, but set the nin-th item to the keyword arg */
        for (i = 0; i < nin; ++i) {
            PyObject *item = PyTuple_GET_ITEM(args, i);
            Py_INCREF(item);
            PyTuple_SET_ITEM(arr_prep_args, i, item);
        }
        Py_INCREF(out);
        PyTuple_SET_ITEM(arr_prep_args, nin, out);
        for (i = nin+1; i < n; ++i) {
            PyObject *item = PyTuple_GET_ITEM(args, i);
            Py_INCREF(item);
            PyTuple_SET_ITEM(arr_prep_args, i, item);
        }

        return arr_prep_args;
    }
}

/*
 * check the floating point status
 *  - errmask: mask of status to check
 *  - extobj: ufunc pyvals object
 *            may be null, in which case the thread global one is fetched
 *  - ufunc_name: name of ufunc
 */
static int
_check_ufunc_fperr(int errmask, PyObject *extobj, const char *ufunc_name) {
    int fperr;
    PyObject *errobj = NULL;
    int ret;
    int first = 1;

    if (!errmask) {
        return 0;
    }
    fperr = PyUFunc_getfperr();
    if (!fperr) {
        return 0;
    }

    /* Get error object globals */
    if (extobj == NULL) {
        extobj = get_global_ext_obj();
    }
    if (_extract_pyvals(extobj, ufunc_name,
                        NULL, NULL, &errobj) < 0) {
        Py_XDECREF(errobj);
        return -1;
    }

    ret = PyUFunc_handlefperr(errmask, errobj, fperr, &first);
    Py_XDECREF(errobj);

    return ret;
}


static int
PyMUFunc_GeneralizedFunction(PyUFuncObject *ufunc,
                        PyObject *args, PyObject *kwds,
                        PyMicArrayObject **op)
{
    int nin, nout;
    int i, j, idim, nop;
    const char *ufunc_name;
    int retval = -1, subok = 1;
    int needs_api = 0;

    PyArray_Descr *dtypes[NPY_MAXARGS];

    /* Use remapped axes for generalized ufunc */
    int broadcast_ndim, iter_ndim;
    int op_axes_arrays[NPY_MAXARGS][NPY_MAXDIMS];
    int *op_axes[NPY_MAXARGS];

    npy_uint32 op_flags[NPY_MAXARGS];
    npy_intp iter_shape[NPY_MAXARGS];
    NpyIter *iter = NULL;
    npy_uint32 iter_flags;
    npy_intp total_problem_size;

    PyArrayObject *op_npy[NPY_MAXARGS];
    int device;

    /* These parameters come from extobj= or from a TLS global */
    int buffersize = 0, errormask = 0;

    /* The selected inner loop */
    PyUFuncGenericFunction innerloop = NULL;
    void *innerloopdata = NULL;
    /* The dimensions which get passed to the inner loop */
    npy_intp inner_dimensions[NPY_MAXDIMS+1];
    /* The strides which get passed to the inner loop */
    npy_intp *inner_strides = NULL;

    /* The sizes of the core dimensions (# entries is ufunc->core_num_dim_ix) */
    npy_intp *core_dim_sizes = inner_dimensions + 1;
    int core_dim_ixs_size;

    /* The __array_prepare__ function to call for each output */
    PyObject *arr_prep[NPY_MAXARGS];
    /*
     * This is either args, or args with the out= parameter from
     * kwds added appropriately.
     */
    PyObject *arr_prep_args = NULL;

    NPY_ORDER order = NPY_KEEPORDER;
    /* Use the default assignment casting rule */
    NPY_CASTING casting = NPY_DEFAULT_ASSIGN_CASTING;
    /* When provided, extobj and typetup contain borrowed references */
    PyObject *extobj = NULL, *type_tup = NULL;

    if (ufunc == NULL) {
        PyErr_SetString(PyExc_ValueError, "function not supported");
        return -1;
    }

    nin = ufunc->nin;
    nout = ufunc->nout;
    nop = nin + nout;

    ufunc_name = ufunc->name ? ufunc->name : "<unnamed mufunc>";

    NPY_UF_DBG_PRINT1("\nEvaluating ufunc %s\n", ufunc_name);

    /* Initialize all the operands and dtypes to NULL */
    for (i = 0; i < nop; ++i) {
        op[i] = NULL;
        dtypes[i] = NULL;
        arr_prep[i] = NULL;
    }

    NPY_UF_DBG_PRINT("Getting arguments\n");

    /* Get all the arguments */
    retval = get_ufunc_arguments(ufunc, args, kwds,
                op, &order, &casting, &extobj,
                &type_tup, &subok, NULL);
    if (retval < 0) {
        goto fail;
    }

    /*
     * Figure out the number of iteration dimensions, which
     * is the broadcast result of all the input non-core
     * dimensions.
     */
    broadcast_ndim = 0;
    for (i = 0; i < nin; ++i) {
        int n = PyMicArray_NDIM(op[i]) - ufunc->core_num_dims[i];
        if (n > broadcast_ndim) {
            broadcast_ndim = n;
        }
    }

    /*
     * Figure out the number of iterator creation dimensions,
     * which is the broadcast dimensions + all the core dimensions of
     * the outputs, so that the iterator can allocate those output
     * dimensions following the rules of order='F', for example.
     */
    iter_ndim = broadcast_ndim;
    for (i = nin; i < nop; ++i) {
        iter_ndim += ufunc->core_num_dims[i];
    }
    if (iter_ndim > NPY_MAXDIMS) {
        PyErr_Format(PyExc_ValueError,
                    "too many dimensions for generalized ufunc %s",
                    ufunc_name);
        retval = -1;
        goto fail;
    }

    /*
     * Validate the core dimensions of all the operands, and collect all of
     * the labelled core dimensions into 'core_dim_sizes'.
     *
     * The behavior has been changed in NumPy 1.10.0, and the following
     * requirements must be fulfilled or an error will be raised:
     *  * Arguments, both input and output, must have at least as many
     *    dimensions as the corresponding number of core dimensions. In
     *    previous versions, 1's were prepended to the shape as needed.
     *  * Core dimensions with same labels must have exactly matching sizes.
     *    In previous versions, core dimensions of size 1 would broadcast
     *    against other core dimensions with the same label.
     *  * All core dimensions must have their size specified by a passed in
     *    input or output argument. In previous versions, core dimensions in
     *    an output argument that were not specified in an input argument,
     *    and whose size could not be inferred from a passed in output
     *    argument, would have their size set to 1.
     */
    for (i = 0; i < ufunc->core_num_dim_ix; ++i) {
        core_dim_sizes[i] = -1;
    }
    for (i = 0; i < nop; ++i) {
        if (op[i] != NULL) {
            int dim_offset = ufunc->core_offsets[i];
            int num_dims = ufunc->core_num_dims[i];
            int core_start_dim = PyMicArray_NDIM(op[i]) - num_dims;

            /* Check if operands have enough dimensions */
            if (core_start_dim < 0) {
                PyErr_Format(PyExc_ValueError,
                        "%s: %s operand %d does not have enough "
                        "dimensions (has %d, gufunc core with "
                        "signature %s requires %d)",
                        ufunc_name, i < nin ? "Input" : "Output",
                        i < nin ? i : i - nin, PyMicArray_NDIM(op[i]),
                        ufunc->core_signature, num_dims);
                retval = -1;
                goto fail;
            }

            /*
             * Make sure every core dimension exactly matches all other core
             * dimensions with the same label.
             */
            for (idim = 0; idim < num_dims; ++idim) {
                int core_dim_index = ufunc->core_dim_ixs[dim_offset+idim];
                npy_intp op_dim_size =
                            PyMicArray_DIM(op[i], core_start_dim+idim);

                if (core_dim_sizes[core_dim_index] == -1) {
                    core_dim_sizes[core_dim_index] = op_dim_size;
                }
                else if (op_dim_size != core_dim_sizes[core_dim_index]) {
                    PyErr_Format(PyExc_ValueError,
                            "%s: %s operand %d has a mismatch in its "
                            "core dimension %d, with gufunc "
                            "signature %s (size %zd is different "
                            "from %zd)",
                            ufunc_name, i < nin ? "Input" : "Output",
                            i < nin ? i : i - nin, idim,
                            ufunc->core_signature, op_dim_size,
                            core_dim_sizes[core_dim_index]);
                    retval = -1;
                    goto fail;
                }
            }
        }
    }

    /*
     * Make sure no core dimension is unspecified.
     */
    for (i = 0; i < ufunc->core_num_dim_ix; ++i) {
        if (core_dim_sizes[i] == -1) {
            break;
        }
    }
    if (i != ufunc->core_num_dim_ix) {
        /*
         * There is at least one core dimension missing, find in which
         * operand it comes up first (it has to be an output operand).
         */
        const int missing_core_dim = i;
        int out_op;
        for (out_op = nin; out_op < nop; ++out_op) {
            int first_idx = ufunc->core_offsets[out_op];
            int last_idx = first_idx + ufunc->core_num_dims[out_op];
            for (i = first_idx; i < last_idx; ++i) {
                if (ufunc->core_dim_ixs[i] == missing_core_dim) {
                    break;
                }
            }
            if (i < last_idx) {
                /* Change index offsets for error message */
                out_op -= nin;
                i -= first_idx;
                break;
            }
        }
        PyErr_Format(PyExc_ValueError,
                     "%s: Output operand %d has core dimension %d "
                     "unspecified, with gufunc signature %s",
                     ufunc_name, out_op, i, ufunc->core_signature);
        retval = -1;
        goto fail;
    }

    /* Fill in the initial part of 'iter_shape' */
    for (idim = 0; idim < broadcast_ndim; ++idim) {
        iter_shape[idim] = -1;
    }

    /* Fill in op_axes for all the operands */
    j = broadcast_ndim;
    core_dim_ixs_size = 0;
    for (i = 0; i < nop; ++i) {
        int n;
        if (op[i]) {
            /*
             * Note that n may be negative if broadcasting
             * extends into the core dimensions.
             */
            n = PyMicArray_NDIM(op[i]) - ufunc->core_num_dims[i];
        }
        else {
            n = broadcast_ndim;
        }
        /* Broadcast all the unspecified dimensions normally */
        for (idim = 0; idim < broadcast_ndim; ++idim) {
            if (idim >= broadcast_ndim - n) {
                op_axes_arrays[i][idim] = idim - (broadcast_ndim - n);
            }
            else {
                op_axes_arrays[i][idim] = -1;
            }
        }

        /* Any output core dimensions shape should be ignored */
        for (idim = broadcast_ndim; idim < iter_ndim; ++idim) {
            op_axes_arrays[i][idim] = -1;
        }

        /* Except for when it belongs to this output */
        if (i >= nin) {
            int dim_offset = ufunc->core_offsets[i];
            int num_dims = ufunc->core_num_dims[i];
            /* Fill in 'iter_shape' and 'op_axes' for this output */
            for (idim = 0; idim < num_dims; ++idim) {
                iter_shape[j] = core_dim_sizes[
                                        ufunc->core_dim_ixs[dim_offset + idim]];
                op_axes_arrays[i][j] = n + idim;
                ++j;
            }
        }

        op_axes[i] = op_axes_arrays[i];
        core_dim_ixs_size += ufunc->core_num_dims[i];
    }

    /* Get the buffersize and errormask */
    if (_get_bufsize_errmask(extobj, ufunc_name, &buffersize, &errormask) < 0) {
        retval = -1;
        goto fail;
    }

    NPY_UF_DBG_PRINT("Finding inner loop\n");


    retval = ufunc->type_resolver(ufunc, casting,
                            (PyArrayObject **)op, type_tup, dtypes);
    if (retval < 0) {
        goto fail;
    }
    /* For the generalized ufunc, we get the loop right away too */
    retval = ufunc->legacy_inner_loop_selector(ufunc, dtypes,
                                    &innerloop, &innerloopdata, &needs_api);
    if (retval < 0) {
        goto fail;
    }

#if NPY_UF_DBG_TRACING
    printf("input types:\n");
    for (i = 0; i < nin; ++i) {
        PyObject_Print((PyObject *)dtypes[i], stdout, 0);
        printf(" ");
    }
    printf("\noutput types:\n");
    for (i = nin; i < nop; ++i) {
        PyObject_Print((PyObject *)dtypes[i], stdout, 0);
        printf(" ");
    }
    printf("\n");
#endif

    if (subok) {
        /* TODO: Do we really need subok? */
        PyErr_SetString(PyExc_ValueError, "Do not support subok");
        goto fail;
        /*
         * Get the appropriate __array_prepare__ function to call
         * for each output
         */
        //_find_array_prepare(args, kwds, arr_prep, nin, nout, 0);

        /* Set up arr_prep_args if a prep function was needed */
        /*
        for (i = 0; i < nout; ++i) {
            if (arr_prep[i] != NULL && arr_prep[i] != Py_None) {
                arr_prep_args = make_arr_prep_args(nin, args, kwds);
                break;
            }
        }
        */
    }

    /* If the loop wants the arrays, provide them */
    if (_does_loop_use_arrays(innerloopdata)) {
        innerloopdata = (void*)op;
    }

    /*
     * Set up the iterator per-op flags.  For generalized ufuncs, we
     * can't do buffering, so must COPY or UPDATEIFCOPY.
     */
    for (i = 0; i < nin; ++i) {
        op_flags[i] = NPY_ITER_READONLY |
                      NPY_ITER_ALIGNED;
        /*
         * If READWRITE flag has been set for this operand,
         * then clear default READONLY flag
         */
        op_flags[i] |= ufunc->op_flags[i];
        if (op_flags[i] & (NPY_ITER_READWRITE | NPY_ITER_WRITEONLY)) {
            op_flags[i] &= ~NPY_ITER_READONLY;
        }
    }
    for (i = nin; i < nop; ++i) {
        op_flags[i] = NPY_ITER_READWRITE|
                      //NPY_ITER_UPDATEIFCOPY|
                      NPY_ITER_ALIGNED|
                      //NPY_ITER_ALLOCATE|
                      NPY_ITER_NO_BROADCAST;
    }

    iter_flags = ufunc->iter_flags |
                 NPY_ITER_MULTI_INDEX |
                 NPY_ITER_REFS_OK |
                 NPY_ITER_REDUCE_OK |
                 NPY_ITER_ZEROSIZE_OK;
    /* Find destination device */
    device = PyMUFunc_GetCommonDevice(nin, op);

    /* Allocate output array */
    for (i = nin; i < nop; ++i) {
        PyMicArrayObject *tmp;
        if (op[i] == NULL) {
            tmp = PyMUFunc_CreateArrayBroadcast(nin, op, dtypes[i]);
            if (tmp == NULL) {
                goto fail;
            }

            op[i] = tmp;
        }
    }

    /* Create temporary PyMicArrayObject * array */
    /* TODO cleanup this step */
    /*
    for (i = 0; i < nop; ++i) {
        op_npy[i] = (PyArrayObject *) op[i];
    }
    */

    /* Create the iterator */
    iter = NpyIter_AdvancedNew(nop, (PyArrayObject **) op, iter_flags,
                           order, NPY_UNSAFE_CASTING, op_flags,
                           dtypes, iter_ndim,
                           op_axes, iter_shape, 0);
    if (iter == NULL) {
        retval = -1;
        goto fail;
    }

    /* Fill in any allocated outputs */
    /*for (i = nin; i < nop; ++i) {
        if (op[i] == NULL) {
            op[i] = NpyIter_GetOperandArray(iter)[i];
            Py_INCREF(op[i]);
        }
    }
    */

    /*
     * Set up the inner strides array. Because we're not doing
     * buffering, the strides are fixed throughout the looping.
     */
    inner_strides = (npy_intp *)PyArray_malloc(
                        NPY_SIZEOF_INTP * (nop+core_dim_ixs_size));
    if (inner_strides == NULL) {
        PyErr_NoMemory();
        retval = -1;
        goto fail;
    }
    /* Copy the strides after the first nop */
    idim = nop;
    for (i = 0; i < nop; ++i) {
        int num_dims = ufunc->core_num_dims[i];
        int core_start_dim = PyMicArray_NDIM(op[i]) - num_dims;
        /*
         * Need to use the arrays in the iterator, not op, because
         * a copy with a different-sized type may have been made.
         */
        //PyArrayObject *arr = NpyIter_GetOperandArray(iter)[i];
        PyMicArrayObject *arr = op[i];
        npy_intp *shape = PyMicArray_SHAPE(arr);
        npy_intp *strides = PyMicArray_STRIDES(arr);
        for (j = 0; j < num_dims; ++j) {
            if (core_start_dim + j >= 0) {
                /*
                 * Force the stride to zero when the shape is 1, sot
                 * that the broadcasting works right.
                 */
                if (shape[core_start_dim + j] != 1) {
                    inner_strides[idim++] = strides[core_start_dim + j];
                } else {
                    inner_strides[idim++] = 0;
                }
            } else {
                inner_strides[idim++] = 0;
            }
        }
    }

    total_problem_size = NpyIter_GetIterSize(iter);
    if (total_problem_size < 0) {
        /*
         * Only used for threading, if negative (this means that it is
         * larger then ssize_t before axes removal) assume that the actual
         * problem is large enough to be threaded usefully.
         */
        total_problem_size = 1000;
    }

    /* Remove all the core output dimensions from the iterator */
    for (i = broadcast_ndim; i < iter_ndim; ++i) {
        if (NpyIter_RemoveAxis(iter, broadcast_ndim) != NPY_SUCCEED) {
            retval = -1;
            goto fail;
        }
    }
    if (NpyIter_RemoveMultiIndex(iter) != NPY_SUCCEED) {
        retval = -1;
        goto fail;
    }
    if (NpyIter_EnableExternalLoop(iter) != NPY_SUCCEED) {
        retval = -1;
        goto fail;
    }

    /*
     * The first nop strides are for the inner loop (but only can
     * copy them after removing the core axes
     */
    memcpy(inner_strides, NpyIter_GetInnerStrideArray(iter),
                                    NPY_SIZEOF_INTP * nop);

#if 0
    printf("strides: ");
    for (i = 0; i < nop+core_dim_ixs_size; ++i) {
        printf("%d ", (int)inner_strides[i]);
    }
    printf("\n");
#endif

    /* Start with the floating-point exception flags cleared */
    PyUFunc_clearfperr();

    NPY_UF_DBG_PRINT("Executing inner loop\n");

    if (NpyIter_GetIterSize(iter) != 0) {
        /* Do the ufunc loop */
        NpyIter_IterNextFunc *iternext;
        char **dataptr;
        npy_intp *count_ptr;
        NPY_BEGIN_THREADS_DEF;

        /* Get the variables needed for the loop */
        iternext = NpyIter_GetIterNext(iter, NULL);
        if (iternext == NULL) {
            retval = -1;
            goto fail;
        }
        dataptr = NpyIter_GetDataPtrArray(iter);
        count_ptr = NpyIter_GetInnerLoopSizePtr(iter);

        if (!needs_api && !NpyIter_IterationNeedsAPI(iter)) {
            NPY_BEGIN_THREADS_THRESHOLDED(total_problem_size);
        }
        do {
            inner_dimensions[0] = *count_ptr;
#pragma omp target device(device) \
            map(to: innerloop, innerloopdata,\
                    inner_dimensions[0:NPY_MAXDIMS+1],\
                    inner_strides[0:nop+core_dim_ixs_size])
            innerloop(NULL, inner_dimensions, inner_strides, innerloopdata);
        } while (iternext(iter));

        if (!needs_api && !NpyIter_IterationNeedsAPI(iter)) {
            NPY_END_THREADS;
        }
    } else {
        /**
         * For each output operand, check if it has non-zero size,
         * and assign the identity if it does. For example, a dot
         * product of two zero-length arrays will be a scalar,
         * which has size one.
         */
        for (i = nin; i < nop; ++i) {
            if (PyMicArray_SIZE(op[i]) != 0) {
                switch (ufunc->identity) {
                    case PyUFunc_Zero:
                        assign_reduce_identity_zero(op[i], NULL);
                        break;
                    case PyUFunc_One:
                        assign_reduce_identity_one(op[i], NULL);
                        break;
                    case PyUFunc_MinusOne:
                        assign_reduce_identity_minusone(op[i], NULL);
                        break;
                    case PyUFunc_None:
                    case PyUFunc_ReorderableNone:
                        PyErr_Format(PyExc_ValueError,
                                "ufunc %s ",
                                ufunc_name);
                        retval = -1;
                        goto fail;
                    default:
                        PyErr_Format(PyExc_ValueError,
                                "ufunc %s has an invalid identity for reduction",
                                ufunc_name);
                        retval = -1;
                        goto fail;
                }
            }
        }
    }

    /* Check whether any errors occurred during the loop */
    if (PyErr_Occurred() ||
        _check_ufunc_fperr(errormask, extobj, ufunc_name) < 0) {
        retval = -1;
        goto fail;
    }

    PyArray_free(inner_strides);
    NpyIter_Deallocate(iter);
    /* The caller takes ownership of all the references in op */
    for (i = 0; i < nop; ++i) {
        Py_XDECREF(dtypes[i]);
        Py_XDECREF(arr_prep[i]);
    }
    Py_XDECREF(type_tup);
    Py_XDECREF(arr_prep_args);

    NPY_UF_DBG_PRINT("Returning Success\n");

    return 0;

fail:
    NPY_UF_DBG_PRINT1("Returning failure code %d\n", retval);
    PyArray_free(inner_strides);
    NpyIter_Deallocate(iter);
    for (i = 0; i < nop; ++i) {
        Py_XDECREF(op[i]);
        op[i] = NULL;
        Py_XDECREF(dtypes[i]);
        Py_XDECREF(arr_prep[i]);
    }
    Py_XDECREF(type_tup);
    Py_XDECREF(arr_prep_args);

    return retval;
}

/*UFUNC_API
 *
 * This generic function is called with the ufunc object, the arguments to it,
 * and an array of (pointers to) PyMicArrayObjects which are NULL.
 *
 * 'op' is an array of at least NPY_MAXARGS PyMicArrayObject *.
 */
NPY_NO_EXPORT int
PyMUFunc_GenericFunction(PyUFuncObject *ufunc,
                        PyObject *args, PyObject *kwds,
                        PyMicArrayObject **op)
{
    int nin, nout;
    int i, nop;
    const char *ufunc_name;
    int retval = -1, subok = 0;
    int need_fancy = 0;

    PyArray_Descr *dtypes[NPY_MAXARGS];

    /* These parameters come from extobj= or from a TLS global */
    int buffersize = 0, errormask = 0;

    /* The mask provided in the 'where=' parameter */
    PyMicArrayObject *wheremask = NULL;

    /* The __array_prepare__ function to call for each output */
    PyObject *arr_prep[NPY_MAXARGS];
    /*
     * This is either args, or args with the out= parameter from
     * kwds added appropriately.
     */
    PyObject *arr_prep_args = NULL;

    int trivial_loop_ok = 0;

    NPY_ORDER order = NPY_KEEPORDER;
    /* Use the default assignment casting rule */
    NPY_CASTING casting = NPY_DEFAULT_ASSIGN_CASTING;
    /* When provided, extobj and typetup contain borrowed references */
    PyObject *extobj = NULL, *type_tup = NULL;

    if (ufunc == NULL) {
        PyErr_SetString(PyExc_ValueError, "function not supported");
        return -1;
    }

    if (ufunc->core_enabled) {
        return PyMUFunc_GeneralizedFunction(ufunc, args, kwds, op);
    }

    nin = ufunc->nin;
    nout = ufunc->nout;
    nop = nin + nout;

    ufunc_name = ufunc->name ? ufunc->name : "<unnamed ufunc>";

    NPY_UF_DBG_PRINT1("\nEvaluating ufunc %s\n", ufunc_name);

    /* Initialize all the operands and dtypes to NULL */
    for (i = 0; i < nop; ++i) {
        op[i] = NULL;
        dtypes[i] = NULL;
        arr_prep[i] = NULL;
    }

    /* Get all the arguments */
    retval = get_ufunc_arguments(ufunc, args, kwds,
                op, &order, &casting, &extobj,
                &type_tup, &subok, &wheremask);
    if (retval < 0) {
        goto fail;
    }

    /* All array have to be in the same device */
    retval = _on_same_device(ufunc, op);
    if (retval < 0) {
        PyErr_SetString(PyExc_ValueError, "All array have to be on the same device");
        goto fail;
    }


    /*
     * Use the masked loop if a wheremask was specified.
     */
    if (wheremask != NULL) {
        need_fancy = 1;
    }

    /* Get the buffersize and errormask */
    if (_get_bufsize_errmask(extobj, ufunc_name, &buffersize, &errormask) < 0) {
        retval = -1;
        goto fail;
    }

    retval = ufunc->type_resolver(ufunc, casting,
                            (PyArrayObject **)op, type_tup, dtypes);
    if (retval < 0) {
        goto fail;
    }

    /* Only do the trivial loop check for the unmasked version. */
    if (!need_fancy) {
        /*
         * This checks whether a trivial loop is ok, making copies of
         * scalar and one dimensional operands if that will help.
         */
        trivial_loop_ok = check_for_trivial_loop(ufunc, op, dtypes, buffersize);
        if (trivial_loop_ok < 0) {
            goto fail;
        }
    }

#if NPY_UF_DBG_TRACING
    printf("input types:\n");
    for (i = 0; i < nin; ++i) {
        PyObject_Print((PyObject *)dtypes[i], stdout, 0);
        printf(" ");
    }
    printf("\noutput types:\n");
    for (i = nin; i < nop; ++i) {
        PyObject_Print((PyObject *)dtypes[i], stdout, 0);
        printf(" ");
    }
    printf("\n");
#endif

    if (subok) {
        PyErr_SetString(PyExc_ValueError, "does not support subok right now");

        goto fail;
    }

    /* Start with the floating-point exception flags cleared */
    PyUFunc_clearfperr();

    /* Do the ufunc loop */
    if (need_fancy) {
        NPY_UF_DBG_PRINT("Executing fancy inner loop\n");

        retval = execute_fancy_ufunc_loop(ufunc, wheremask,
                            op, dtypes, order,
                            buffersize, arr_prep, arr_prep_args);
    }
    else {
        NPY_UF_DBG_PRINT("Executing legacy inner loop\n");

        retval = execute_legacy_ufunc_loop(ufunc, trivial_loop_ok,
                            op, dtypes, order,
                            buffersize, arr_prep, arr_prep_args);
    }
    if (retval < 0) {
        goto fail;
    }

    /* Check whether any errors occurred during the loop */
    if (PyErr_Occurred() ||
        _check_ufunc_fperr(errormask, extobj, ufunc_name) < 0) {
        retval = -1;
        goto fail;
    }


    /* The caller takes ownership of all the references in op */
    for (i = 0; i < nop; ++i) {
        Py_XDECREF(dtypes[i]);
        Py_XDECREF(arr_prep[i]);
    }
    Py_XDECREF(type_tup);
    Py_XDECREF(arr_prep_args);
    Py_XDECREF(wheremask);

    NPY_UF_DBG_PRINT("Returning Success\n");

    return 0;

fail:
    NPY_UF_DBG_PRINT1("Returning failure code %d\n", retval);
    for (i = 0; i < nop; ++i) {
        Py_XDECREF(op[i]);
        op[i] = NULL;
        Py_XDECREF(dtypes[i]);
        Py_XDECREF(arr_prep[i]);
    }
    Py_XDECREF(type_tup);
    Py_XDECREF(arr_prep_args);
    Py_XDECREF(wheremask);

    return retval;
}

/*
 * Given the output type, finds the specified binary op.  The
 * ufunc must have nin==2 and nout==1.  The function may modify
 * otype if the given type isn't found.
 *
 * Returns 0 on success, -1 on failure.
 */
static int
get_binary_op_function(PyUFuncObject *ufunc, int *otype,
                        PyUFuncGenericFunction *out_innerloop,
                        void **out_innerloopdata)
{
    int i;
    PyUFunc_Loop1d *funcdata;

    NPY_UF_DBG_PRINT1("Getting binary op function for type number %d\n",
                                *otype);

    /* If the type is custom and there are userloops, search for it here */
    if (ufunc->userloops != NULL && PyTypeNum_ISUSERDEF(*otype)) {
        PyObject *key, *obj;
        key = PyInt_FromLong(*otype);
        if (key == NULL) {
            return -1;
        }
        obj = PyDict_GetItem(ufunc->userloops, key);
        Py_DECREF(key);
        if (obj != NULL) {
            funcdata = (PyUFunc_Loop1d *)NpyCapsule_AsVoidPtr(obj);
            while (funcdata != NULL) {
                int *types = funcdata->arg_types;

                if (types[0] == *otype && types[1] == *otype &&
                                                types[2] == *otype) {
                    *out_innerloop = funcdata->func;
                    *out_innerloopdata = funcdata->data;
                    return 0;
                }

                funcdata = funcdata->next;
            }
        }
    }

    /* Search for a function with compatible inputs */
    for (i = 0; i < ufunc->ntypes; ++i) {
        char *types = ufunc->types + i*ufunc->nargs;

        NPY_UF_DBG_PRINT3("Trying loop with signature %d %d -> %d\n",
                                types[0], types[1], types[2]);

        if (PyArray_CanCastSafely(*otype, types[0]) &&
                    types[0] == types[1] &&
                    (*otype == NPY_OBJECT || types[0] != NPY_OBJECT)) {
            /* If the signature is "xx->x", we found the loop */
            if (types[2] == types[0]) {
                *out_innerloop = ufunc->functions[i];
                *out_innerloopdata = ufunc->data[i];
                *otype = types[0];
                return 0;
            }
            /*
             * Otherwise, we found the natural type of the reduction,
             * replace otype and search again
             */
            else {
                *otype = types[2];
                break;
            }
        }
    }

    /* Search for the exact function */
    for (i = 0; i < ufunc->ntypes; ++i) {
        char *types = ufunc->types + i*ufunc->nargs;

        if (PyArray_CanCastSafely(*otype, types[0]) &&
                    types[0] == types[1] &&
                    types[1] == types[2] &&
                    (*otype == NPY_OBJECT || types[0] != NPY_OBJECT)) {
            /* Since the signature is "xx->x", we found the loop */
            *out_innerloop = ufunc->functions[i];
            *out_innerloopdata = ufunc->data[i];
            *otype = types[0];
            return 0;
        }
    }

    return -1;
}

static int
reduce_type_resolver(PyUFuncObject *ufunc, PyMicArrayObject *arr,
                        PyArray_Descr *odtype, PyArray_Descr **out_dtype)
{
    int i, retcode;
    PyArrayObject *op[3] = {(PyArrayObject *)arr, (PyArrayObject *)arr, NULL};
    PyArray_Descr *dtypes[3] = {NULL, NULL, NULL};
    const char *ufunc_name = ufunc->name ? ufunc->name : "(unknown)";
    PyObject *type_tup = NULL;

    *out_dtype = NULL;

    /*
     * If odtype is specified, make a type tuple for the type
     * resolution.
     */
    if (odtype != NULL) {
        type_tup = PyTuple_Pack(3, odtype, odtype, Py_None);
        if (type_tup == NULL) {
            return -1;
        }
    }

    /* Use the type resolution function to find our loop */
    retcode = ufunc->type_resolver(
                        ufunc, NPY_UNSAFE_CASTING,
                        op, type_tup, dtypes);
    Py_DECREF(type_tup);
    if (retcode == -1) {
        return -1;
    }
    else if (retcode == -2) {
        PyErr_Format(PyExc_RuntimeError,
                "type resolution returned NotImplemented to "
                "reduce ufunc %s", ufunc_name);
        return -1;
    }

    /*
     * The first two type should be equivalent. Because of how
     * reduce has historically behaved in NumPy, the return type
     * could be different, and it is the return type on which the
     * reduction occurs.
     */
    if (!PyArray_EquivTypes(dtypes[0], dtypes[1])) {
        for (i = 0; i < 3; ++i) {
            Py_DECREF(dtypes[i]);
        }
        PyErr_Format(PyExc_RuntimeError,
                "could not find a type resolution appropriate for "
                "reduce ufunc %s", ufunc_name);
        return -1;
    }

    Py_DECREF(dtypes[0]);
    Py_DECREF(dtypes[1]);
    *out_dtype = dtypes[2];

    return 0;
}

static int
assign_reduce_identity_zero(PyMicArrayObject *result, void *NPY_UNUSED(data))
{
    return PyMicArray_FillWithScalar(result, PyArrayScalar_False);
}

static int
assign_reduce_identity_one(PyMicArrayObject *result, void *NPY_UNUSED(data))
{
    return PyMicArray_FillWithScalar(result, PyArrayScalar_True);
}

static int
assign_reduce_identity_minusone(PyMicArrayObject *result, void *NPY_UNUSED(data))
{
    static PyObject *MinusOne = NULL;

    if (MinusOne == NULL) {
        if ((MinusOne = PyInt_FromLong(-1)) == NULL) {
            return -1;
        }
    }
    return PyMicArray_FillWithScalar(result, MinusOne);
}

static int
reduce_loop(NpyIter *iter, char **dataptrs, npy_intp *strides,
            npy_intp *countptr, NpyIter_IterNextFunc *iternext,
            int needs_api, npy_intp skip_first_count, void *data, int device)
{
    /* TODO */
    PyArray_Descr *dtypes[3], **iter_dtypes;
    PyUFuncObject *ufunc = (PyUFuncObject *)data;
    char *dataptrs_copy[3];
    npy_intp strides_copy[3];

    /* The normal selected inner loop */
    PyUFuncGenericFunction innerloop = NULL;
    void *innerloopdata = NULL;

    NPY_BEGIN_THREADS_DEF;

    /* Get the inner loop */
    iter_dtypes = NpyIter_GetDescrArray(iter);
    dtypes[0] = iter_dtypes[0];
    dtypes[1] = iter_dtypes[1];
    dtypes[2] = iter_dtypes[0];
    if (ufunc->legacy_inner_loop_selector(ufunc, dtypes,
                            &innerloop, &innerloopdata, &needs_api) < 0) {
        return -1;
    }

    NPY_BEGIN_THREADS_NDITER(iter);

    if (skip_first_count > 0) {
        do {
            npy_intp count = *countptr;

            /* Skip any first-visit elements */
            if (NpyIter_IsFirstVisit(iter, 0)) {
                if (strides[0] == 0) {
                    --count;
                    --skip_first_count;
                    dataptrs[1] += strides[1];
                }
                else {
                    skip_first_count -= count;
                    count = 0;
                }
            }

            /* Turn the two items into three for the inner loop */
            dataptrs_copy[0] = dataptrs[0];
            dataptrs_copy[1] = dataptrs[1];
            dataptrs_copy[2] = dataptrs[0];
            strides_copy[0] = strides[0];
            strides_copy[1] = strides[1];
            strides_copy[2] = strides[0];
#pragma omp target device(device) \
            map(to:innerloop, count, strides_copy[0:3], innerloopdata)
            innerloop(NULL, &count,
                        strides_copy, innerloopdata);

            /* Jump to the faster loop when skipping is done */
            if (skip_first_count == 0) {
                if (iternext(iter)) {
                    break;
                }
                else {
                    goto finish_loop;
                }
            }
        } while (iternext(iter));
    }
    do {
        /* Turn the two items into three for the inner loop */
        dataptrs_copy[0] = dataptrs[0];
        dataptrs_copy[1] = dataptrs[1];
        dataptrs_copy[2] = dataptrs[0];
        strides_copy[0] = strides[0];
        strides_copy[1] = strides[1];
        strides_copy[2] = strides[0];
#pragma omp target device(device) \
        map(to: innerloop, countptr[0:1], strides_copy[0:3], innerloopdata)
        innerloop(NULL, countptr,
                    strides_copy, innerloopdata);
    } while (iternext(iter));

finish_loop:
    NPY_END_THREADS;

    return (needs_api && PyErr_Occurred()) ? -1 : 0;
}

/*
 * The implementation of the reduction operators with the new iterator
 * turned into a bit of a long function here, but I think the design
 * of this part needs to be changed to be more like einsum, so it may
 * not be worth refactoring it too much.  Consider this timing:
 *
 * >>> a = arange(10000)
 *
 * >>> timeit sum(a)
 * 10000 loops, best of 3: 17 us per loop
 *
 * >>> timeit einsum("i->",a)
 * 100000 loops, best of 3: 13.5 us per loop
 *
 * The axes must already be bounds-checked by the calling function,
 * this function does not validate them.
 */
static PyMicArrayObject *
PyMUFunc_Reduce(PyUFuncObject *ufunc, PyMicArrayObject *arr, PyMicArrayObject *out,
        int naxes, int *axes, PyArray_Descr *odtype, int keepdims)
{
    /* TODO: implement this */
   return NULL;
}


static PyObject *
PyUFunc_Accumulate(PyUFuncObject *ufunc, PyArrayObject *arr, PyArrayObject *out,
                   int axis, int otype)
{
   /* TODO:implement this */
   return NULL;
}

/*
 * Reduceat performs a reduce over an axis using the indices as a guide
 *
 * op.reduceat(array,indices)  computes
 * op.reduce(array[indices[i]:indices[i+1]]
 * for i=0..end with an implicit indices[i+1]=len(array)
 * assumed when i=end-1
 *
 * if indices[i+1] <= indices[i]+1
 * then the result is array[indices[i]] for that value
 *
 * op.accumulate(array) is the same as
 * op.reduceat(array,indices)[::2]
 * where indices is range(len(array)-1) with a zero placed in every other sample
 * indices = zeros(len(array)*2-1)
 * indices[1::2] = range(1,len(array))
 *
 * output shape is based on the size of indices
 */
static PyObject *
PyUFunc_Reduceat(PyUFuncObject *ufunc, PyArrayObject *arr, PyArrayObject *ind,
                 PyArrayObject *out, int axis, int otype)
{
    //TODO: Implement this
    return NULL;
}


/*
 * This code handles reduce, reduceat, and accumulate
 * (accumulate and reduce are special cases of the more general reduceat
 * but they are handled separately for speed)
 */
static PyObject *
PyMUFunc_GenericReduction(PyUFuncObject *ufunc, PyObject *args,
                         PyObject *kwds, int operation)
{
    /* TODO */
    return NULL;
}


static PyObject *
mufunc_generic_call(PyUFuncObject *ufunc, PyObject *args, PyObject *kwds)
{
    int i;
    PyTupleObject *ret;
    PyMicArrayObject *mps[NPY_MAXARGS];
    PyObject *retobj[NPY_MAXARGS];
    PyObject *wraparr[NPY_MAXARGS];
    PyObject *res;
    PyObject *override = NULL;
    int errval;

    /*
     * Initialize all array objects to NULL to make cleanup easier
     * if something goes wrong.
     */
    for (i = 0; i < ufunc->nargs; i++) {
        mps[i] = NULL;
    }

    errval = PyMUFunc_GenericFunction(ufunc, args, kwds, mps);
    if (errval < 0) {
        for (i = 0; i < ufunc->nargs; i++) {
            PyArray_XDECREF_ERR((PyArrayObject *)mps[i]);
        }
        if (errval == -1) {
            return NULL;
        }
        else if (ufunc->nin == 2 && ufunc->nout == 1) {
            /*
             * For array_richcompare's benefit -- see the long comment in
             * get_ufunc_arguments.
             */
            Py_INCREF(Py_NotImplemented);
            return Py_NotImplemented;
        }
        else {
            PyErr_SetString(PyExc_TypeError,
                            "XX can't happen, please report a bug XX");
            return NULL;
        }
    }

    /* Free the input references */
    for (i = 0; i < ufunc->nin; i++) {
        Py_XDECREF(mps[i]);
    }

    /*
     * Use __array_wrap__ on all outputs
     * if present on one of the input arguments.
     * If present for multiple inputs:
     * use __array_wrap__ of input object with largest
     * __array_priority__ (default = 0.0)
     *
     * Exception:  we should not wrap outputs for items already
     * passed in as output-arguments.  These items should either
     * be left unwrapped or wrapped by calling their own __array_wrap__
     * routine.
     *
     * For each output argument, wrap will be either
     * NULL --- call PyArray_Return() -- default if no output arguments given
     * None --- array-object passed in don't call PyArray_Return
     * method --- the __array_wrap__ method to call.
     */

    /* wrap outputs */
    for (i = 0; i < ufunc->nout; i++) {
        int j = ufunc->nin+i;

        /* default behavior */
        retobj[i] = PyMicArray_Return(mps[j]);
    }

    if (ufunc->nout == 1) {
        return retobj[0];
    }
    else {
        ret = (PyTupleObject *)PyTuple_New(ufunc->nout);
        for (i = 0; i < ufunc->nout; i++) {
            PyTuple_SET_ITEM(ret, i, retobj[i]);
        }
        return (PyObject *)ret;
    }
}

NPY_NO_EXPORT PyObject *
ufunc_geterr(PyObject *NPY_UNUSED(dummy), PyObject *args)
{
    PyObject *thedict;
    PyObject *res;

    if (!PyArg_ParseTuple(args, "")) {
        return NULL;
    }
    thedict = PyThreadState_GetDict();
    if (thedict == NULL) {
        thedict = PyEval_GetBuiltins();
    }
    res = PyDict_GetItem(thedict, mpy_um_str_pyvals_name);
    if (res != NULL) {
        Py_INCREF(res);
        return res;
    }
    /* Construct list of defaults */
    res = PyList_New(3);
    if (res == NULL) {
        return NULL;
    }
    PyList_SET_ITEM(res, 0, PyInt_FromLong(NPY_BUFSIZE));
    PyList_SET_ITEM(res, 1, PyInt_FromLong(UFUNC_ERR_DEFAULT));
    PyList_SET_ITEM(res, 2, Py_None); Py_INCREF(Py_None);
    return res;
}

#if USE_USE_DEFAULTS==1
/*
 * This is a strategy to buy a little speed up and avoid the dictionary
 * look-up in the default case.  It should work in the presence of
 * threads.  If it is deemed too complicated or it doesn't actually work
 * it could be taken out.
 */
static int
ufunc_update_use_defaults(void)
{
    PyObject *errobj = NULL;
    int errmask, bufsize;
    int res;

    PyUFunc_NUM_NODEFAULTS += 1;
    res = PyUFunc_GetPyValues("test", &bufsize, &errmask, &errobj);
    PyUFunc_NUM_NODEFAULTS -= 1;
    if (res < 0) {
        Py_XDECREF(errobj);
        return -1;
    }
    if ((errmask != UFUNC_ERR_DEFAULT) || (bufsize != NPY_BUFSIZE)
            || (PyTuple_GET_ITEM(errobj, 1) != Py_None)) {
        PyUFunc_NUM_NODEFAULTS += 1;
    }
    else if (PyUFunc_NUM_NODEFAULTS > 0) {
        PyUFunc_NUM_NODEFAULTS -= 1;
    }
    Py_XDECREF(errobj);
    return 0;
}
#endif

NPY_NO_EXPORT PyObject *
ufunc_seterr(PyObject *NPY_UNUSED(dummy), PyObject *args)
{
    PyObject *thedict;
    int res;
    PyObject *val;
    static char *msg = "Error object must be a list of length 3";

    if (!PyArg_ParseTuple(args, "O", &val)) {
        return NULL;
    }
    if (!PyList_CheckExact(val) || PyList_GET_SIZE(val) != 3) {
        PyErr_SetString(PyExc_ValueError, msg);
        return NULL;
    }
    thedict = PyThreadState_GetDict();
    if (thedict == NULL) {
        thedict = PyEval_GetBuiltins();
    }
    res = PyDict_SetItem(thedict, mpy_um_str_pyvals_name, val);
    if (res < 0) {
        return NULL;
    }
#if USE_USE_DEFAULTS==1
    if (ufunc_update_use_defaults() < 0) {
        return NULL;
    }
#endif
    Py_RETURN_NONE;
}

/*UFUNC_API*/
NPY_NO_EXPORT PyObject *
PyMUFunc_FromFuncAndData(PyUFuncGenericFunction *func, void **data,
                        char *types, int ntypes,
                        int nin, int nout, int identity,
                        const char *name, const char *doc, int unused)
{
    return PyMUFunc_FromFuncAndDataAndSignature(func, data, types, ntypes,
        nin, nout, identity, name, doc, 0, NULL);
}

/*UFUNC_API*/
NPY_NO_EXPORT PyObject *
PyMUFunc_FromFuncAndDataAndSignature(PyUFuncGenericFunction *func, void **data,
                                     char *types, int ntypes,
                                     int nin, int nout, int identity,
                                     const char *name, const char *doc,
                                     int unused, const char *signature)
{
    PyUFuncObject *ufunc;

    ufunc = (PyUFuncObject *) PyUFunc_FromFuncAndDataAndSignature(func, data,
                                        types, ntypes, nin, nout, identity,
                                        name, doc, unused, signature);

    if (ufunc != NULL) {
        /* Modify objec_type to PyMUFunc_Type */
        ufunc->ob_type = &PyMUFunc_Type;
    }

    return (PyObject *)ufunc;
}

static int
_does_loop_use_arrays(void *data)
{
    return (data == PyUFunc_SetUsesArraysAsData);
}

/*
 * This is the first-part of the CObject structure.
 *
 * I don't think this will change, but if it should, then
 * this needs to be fixed.  The exposed C-API was insufficient
 * because I needed to replace the pointer and it wouldn't
 * let me with a destructor set (even though it works fine
 * with the destructor).
 */
typedef struct {
    PyObject_HEAD
    void *c_obj;
} _simple_cobj;

#define _SETCPTR(cobj, val) ((_simple_cobj *)(cobj))->c_obj = (val)

/* return 1 if arg1 > arg2, 0 if arg1 == arg2, and -1 if arg1 < arg2 */
static int
cmp_arg_types(int *arg1, int *arg2, int n)
{
    for (; n > 0; n--, arg1++, arg2++) {
        if (PyArray_EquivTypenums(*arg1, *arg2)) {
            continue;
        }
        if (PyArray_CanCastSafely(*arg1, *arg2)) {
            return -1;
        }
        return 1;
    }
    return 0;
}

/*
 * This frees the linked-list structure when the CObject
 * is destroyed (removed from the internal dictionary)
*/
static NPY_INLINE void
_free_loop1d_list(PyUFunc_Loop1d *data)
{
    int i;

    while (data != NULL) {
        PyUFunc_Loop1d *next = data->next;
        PyArray_free(data->arg_types);

        if (data->arg_dtypes != NULL) {
            for (i = 0; i < data->nargs; i++) {
                Py_DECREF(data->arg_dtypes[i]);
            }
            PyArray_free(data->arg_dtypes);
        }

        PyArray_free(data);
        data = next;
    }
}

#if PY_VERSION_HEX >= 0x03000000
static void
_loop1d_list_free(PyObject *ptr)
{
    PyUFunc_Loop1d *data = (PyUFunc_Loop1d *)PyCapsule_GetPointer(ptr, NULL);
    _free_loop1d_list(data);
}
#else
static void
_loop1d_list_free(void *ptr)
{
    PyUFunc_Loop1d *data = (PyUFunc_Loop1d *)ptr;
    _free_loop1d_list(data);
}
#endif


#undef _SETCPTR


static void
mufunc_dealloc(PyUFuncObject *ufunc)
{
    PyArray_free(ufunc->core_num_dims);
    PyArray_free(ufunc->core_dim_ixs);
    PyArray_free(ufunc->core_offsets);
    PyArray_free(ufunc->core_signature);
    PyArray_free(ufunc->ptr);
    PyArray_free(ufunc->op_flags);
    Py_XDECREF(ufunc->userloops);
    Py_XDECREF(ufunc->obj);
    PyArray_free(ufunc);
}

static PyObject *
mufunc_repr(PyUFuncObject *ufunc)
{
    return PyUString_FromFormat("<mufunc '%s'>", ufunc->name);
}


/******************************************************************************
 ***                          UFUNC METHODS                                 ***
 *****************************************************************************/


/*
 * op.outer(a,b) is equivalent to op(a[:,NewAxis,NewAxis,etc.],b)
 * where a has b.ndim NewAxis terms appended.
 *
 * The result has dimensions a.ndim + b.ndim
 */
static PyObject *
mufunc_outer(PyUFuncObject *ufunc, PyObject *args, PyObject *kwds)
{
    //TODO
    int i;
    int errval;
    PyObject *override = NULL;
    PyObject *ret;
    PyArrayObject *ap1 = NULL, *ap2 = NULL, *ap_new = NULL;
    PyObject *new_args, *tmp;
    PyObject *shape1, *shape2, *newshape;

    if (ufunc->core_enabled) {
        PyErr_Format(PyExc_TypeError,
                     "method outer is not allowed in ufunc with non-trivial"\
                     " signature");
        return NULL;
    }

    if (ufunc->nin != 2) {
        PyErr_SetString(PyExc_ValueError,
                        "outer product only supported "\
                        "for binary functions");
        return NULL;
    }

    if (PySequence_Length(args) != 2) {
        PyErr_SetString(PyExc_TypeError, "exactly two arguments expected");
        return NULL;
    }


    tmp = PySequence_GetItem(args, 0);
    if (tmp == NULL) {
        return NULL;
    }
    ap1 = (PyArrayObject *) PyArray_FromObject(tmp, NPY_NOTYPE, 0, 0);
    Py_DECREF(tmp);
    if (ap1 == NULL) {
        return NULL;
    }
    tmp = PySequence_GetItem(args, 1);
    if (tmp == NULL) {
        return NULL;
    }
    ap2 = (PyArrayObject *)PyArray_FromObject(tmp, NPY_NOTYPE, 0, 0);
    Py_DECREF(tmp);
    if (ap2 == NULL) {
        Py_DECREF(ap1);
        return NULL;
    }
    /* Construct new shape tuple */
    shape1 = PyTuple_New(PyArray_NDIM(ap1));
    if (shape1 == NULL) {
        goto fail;
    }
    for (i = 0; i < PyArray_NDIM(ap1); i++) {
        PyTuple_SET_ITEM(shape1, i,
                PyLong_FromLongLong((npy_longlong)PyArray_DIMS(ap1)[i]));
    }
    shape2 = PyTuple_New(PyArray_NDIM(ap2));
    for (i = 0; i < PyArray_NDIM(ap2); i++) {
        PyTuple_SET_ITEM(shape2, i, PyInt_FromLong((long) 1));
    }
    if (shape2 == NULL) {
        Py_DECREF(shape1);
        goto fail;
    }
    newshape = PyNumber_Add(shape1, shape2);
    Py_DECREF(shape1);
    Py_DECREF(shape2);
    if (newshape == NULL) {
        goto fail;
    }
    ap_new = (PyArrayObject *)PyArray_Reshape(ap1, newshape);
    Py_DECREF(newshape);
    if (ap_new == NULL) {
        goto fail;
    }
    new_args = Py_BuildValue("(OO)", ap_new, ap2);
    Py_DECREF(ap1);
    Py_DECREF(ap2);
    Py_DECREF(ap_new);
    ret = mufunc_generic_call(ufunc, new_args, kwds);
    Py_DECREF(new_args);
    return ret;

 fail:
    Py_XDECREF(ap1);
    Py_XDECREF(ap2);
    Py_XDECREF(ap_new);
    return NULL;
}


static PyObject *
mufunc_reduce(PyUFuncObject *ufunc, PyObject *args, PyObject *kwds)
{
    int errval;
    PyObject *override = NULL;

    /* `nin`, the last arg, is unused. So we put 0. */
    return PyMUFunc_GenericReduction(ufunc, args, kwds, UFUNC_REDUCE);
}

static PyObject *
mufunc_accumulate(PyUFuncObject *ufunc, PyObject *args, PyObject *kwds)
{
    int errval;
    PyObject *override = NULL;

    return PyMUFunc_GenericReduction(ufunc, args, kwds, UFUNC_ACCUMULATE);
}

static PyObject *
mufunc_reduceat(PyUFuncObject *ufunc, PyObject *args, PyObject *kwds)
{
    int errval;
    PyObject *override = NULL;

    return PyMUFunc_GenericReduction(ufunc, args, kwds, UFUNC_REDUCEAT);
}

/* Helper for ufunc_at, below */
static NPY_INLINE PyMicArrayObject *
new_array_op(PyMicArrayObject *op_array, char *data)
{
    npy_intp dims[1] = {1};
    /* TODO: get default device */
    PyObject *r = PyMicArray_NewFromDescr(-1, &PyMicArray_Type, PyMicArray_DESCR(op_array),
                                       1, dims, NULL, data,
                                       NPY_ARRAY_WRITEABLE, NULL);
    return (PyMicArrayObject *)r;
}

/*
 * Call ufunc only on selected array items and store result in first operand.
 * For add ufunc, method call is equivalent to op1[idx] += op2 with no
 * buffering of the first operand.
 * Arguments:
 * op1 - First operand to ufunc
 * idx - Indices that are applied to first operand. Equivalent to op1[idx].
 * op2 - Second operand to ufunc (if needed). Must be able to broadcast
 *       over first operand.
 */
static PyObject *
mufunc_at(PyUFuncObject *ufunc, PyObject *args)
{
    //TODO
    return NULL;
}


static struct PyMethodDef mufunc_methods[] = {
    /*{"reduce",
        (PyCFunction)mufunc_reduce,
        METH_VARARGS | METH_KEYWORDS, NULL },
    {"accumulate",
        (PyCFunction)mufunc_accumulate,
        METH_VARARGS | METH_KEYWORDS, NULL },
    {"reduceat",
        (PyCFunction)mufunc_reduceat,
        METH_VARARGS | METH_KEYWORDS, NULL },
    {"outer",
        (PyCFunction)mufunc_outer,
        METH_VARARGS | METH_KEYWORDS, NULL},
    {"at",
        (PyCFunction)mufunc_at,
        METH_VARARGS, NULL},*/
    {NULL, NULL, 0, NULL}           /* sentinel */
};


/******************************************************************************
 ***                           UFUNC GETSET                                 ***
 *****************************************************************************/


/* construct the string y1,y2,...,yn */
static PyObject *
_makeargs(int num, char *ltr, int null_if_none)
{
    PyObject *str;
    int i;

    switch (num) {
    case 0:
        if (null_if_none) {
            return NULL;
        }
        return PyString_FromString("");
    case 1:
        return PyString_FromString(ltr);
    }
    str = PyString_FromFormat("%s1, %s2", ltr, ltr);
    for (i = 3; i <= num; ++i) {
        PyString_ConcatAndDel(&str, PyString_FromFormat(", %s%d", ltr, i));
    }
    return str;
}

static char
_typecharfromnum(int num) {
    PyArray_Descr *descr;
    char ret;

    descr = PyArray_DescrFromType(num);
    ret = descr->type;
    Py_DECREF(descr);
    return ret;
}

static PyObject *
ufunc_get_doc(PyUFuncObject *ufunc)
{
    /*
     * Put docstring first or FindMethod finds it... could so some
     * introspection on name and nin + nout to automate the first part
     * of it the doc string shouldn't need the calling convention
     * construct name(x1, x2, ...,[ out1, out2, ...]) __doc__
     */
    PyObject *outargs, *inargs, *doc;
    outargs = _makeargs(ufunc->nout, "out", 1);
    inargs = _makeargs(ufunc->nin, "x", 0);

    if (ufunc->doc == NULL) {
        if (outargs == NULL) {
            doc = PyUString_FromFormat("%s(%s)\n\n",
                                        ufunc->name,
                                        PyString_AS_STRING(inargs));
        }
        else {
            doc = PyUString_FromFormat("%s(%s[, %s])\n\n",
                                        ufunc->name,
                                        PyString_AS_STRING(inargs),
                                        PyString_AS_STRING(outargs));
            Py_DECREF(outargs);
        }
    }
    else {
        if (outargs == NULL) {
            doc = PyUString_FromFormat("%s(%s)\n\n%s",
                                       ufunc->name,
                                       PyString_AS_STRING(inargs),
                                       ufunc->doc);
        }
        else {
            doc = PyUString_FromFormat("%s(%s[, %s])\n\n%s",
                                       ufunc->name,
                                       PyString_AS_STRING(inargs),
                                       PyString_AS_STRING(outargs),
                                       ufunc->doc);
            Py_DECREF(outargs);
        }
    }
    Py_DECREF(inargs);
    return doc;
}

static PyObject *
ufunc_get_nin(PyUFuncObject *ufunc)
{
    return PyInt_FromLong(ufunc->nin);
}

static PyObject *
ufunc_get_nout(PyUFuncObject *ufunc)
{
    return PyInt_FromLong(ufunc->nout);
}

static PyObject *
ufunc_get_nargs(PyUFuncObject *ufunc)
{
    return PyInt_FromLong(ufunc->nin + ufunc->nout);
}

static PyObject *
ufunc_get_ntypes(PyUFuncObject *ufunc)
{
    return PyInt_FromLong(ufunc->ntypes);
}

static PyObject *
ufunc_get_types(PyUFuncObject *ufunc)
{
    /* return a list with types grouped input->output */
    PyObject *list;
    PyObject *str;
    int k, j, n, nt = ufunc->ntypes;
    int ni = ufunc->nin;
    int no = ufunc->nout;
    char *t;
    list = PyList_New(nt);
    if (list == NULL) {
        return NULL;
    }
    t = PyArray_malloc(no+ni+2);
    n = 0;
    for (k = 0; k < nt; k++) {
        for (j = 0; j<ni; j++) {
            t[j] = _typecharfromnum(ufunc->types[n]);
            n++;
        }
        t[ni] = '-';
        t[ni+1] = '>';
        for (j = 0; j < no; j++) {
            t[ni + 2 + j] = _typecharfromnum(ufunc->types[n]);
            n++;
        }
        str = PyUString_FromStringAndSize(t, no + ni + 2);
        PyList_SET_ITEM(list, k, str);
    }
    PyArray_free(t);
    return list;
}

static PyObject *
ufunc_get_name(PyUFuncObject *ufunc)
{
    return PyUString_FromString(ufunc->name);
}

static PyObject *
ufunc_get_identity(PyUFuncObject *ufunc)
{
    switch(ufunc->identity) {
    case PyUFunc_One:
        return PyInt_FromLong(1);
    case PyUFunc_Zero:
        return PyInt_FromLong(0);
    case PyUFunc_MinusOne:
        return PyInt_FromLong(-1);
    }
    Py_RETURN_NONE;
}

static PyObject *
ufunc_get_signature(PyUFuncObject *ufunc)
{
    if (!ufunc->core_enabled) {
        Py_RETURN_NONE;
    }
    return PyUString_FromString(ufunc->core_signature);
}

#undef _typecharfromnum

/*
 * Docstring is now set from python
 * static char *Ufunctype__doc__ = NULL;
 */
static PyGetSetDef mufunc_getset[] = {
    {"__doc__",
        (getter)ufunc_get_doc,
        NULL, NULL, NULL},
    {"nin",
        (getter)ufunc_get_nin,
        NULL, NULL, NULL},
    {"nout",
        (getter)ufunc_get_nout,
        NULL, NULL, NULL},
    {"nargs",
        (getter)ufunc_get_nargs,
        NULL, NULL, NULL},
    {"ntypes",
        (getter)ufunc_get_ntypes,
        NULL, NULL, NULL},
    {"types",
        (getter)ufunc_get_types,
        NULL, NULL, NULL},
    {"__name__",
        (getter)ufunc_get_name,
        NULL, NULL, NULL},
    {"identity",
        (getter)ufunc_get_identity,
        NULL, NULL, NULL},
    {"signature",
        (getter)ufunc_get_signature,
        NULL, NULL, NULL},
    {NULL, NULL, NULL, NULL, NULL},  /* Sentinel */
};


/******************************************************************************
 ***                        UFUNC TYPE OBJECT                               ***
 *****************************************************************************/

NPY_NO_EXPORT PyTypeObject PyMUFunc_Type = {
#if defined(NPY_PY3K)
    PyVarObject_HEAD_INIT(NULL, 0)
#else
    PyObject_HEAD_INIT(NULL)
    0,                                          /* ob_size */
#endif
    "micpy.mufunc",                              /* tp_name */
    sizeof(PyUFuncObject),                      /* tp_basicsize */
    0,                                          /* tp_itemsize */
    /* methods */
    (destructor)mufunc_dealloc,                  /* tp_dealloc */
    0,                                          /* tp_print */
    0,                                          /* tp_getattr */
    0,                                          /* tp_setattr */
#if defined(NPY_PY3K)
    0,                                          /* tp_reserved */
#else
    0,                                          /* tp_compare */
#endif
    (reprfunc)mufunc_repr,                      /* tp_repr */
    0,                                          /* tp_as_number */
    0,                                          /* tp_as_sequence */
    0,                                          /* tp_as_mapping */
    0,                                          /* tp_hash */
    (ternaryfunc)mufunc_generic_call,           /* tp_call */
    (reprfunc)mufunc_repr,                      /* tp_str */
    0,                                          /* tp_getattro */
    0,                                          /* tp_setattro */
    0,                                          /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT,                         /* tp_flags */
    0,                                          /* tp_doc */
    0,                                          /* tp_traverse */
    0,                                          /* tp_clear */
    0,                                          /* tp_richcompare */
    0,                                          /* tp_weaklistoffset */
    0,                                          /* tp_iter */
    0,                                          /* tp_iternext */
    mufunc_methods,                             /* tp_methods */
    0,                                          /* tp_members */
    mufunc_getset,                              /* tp_getset */
    0,                                          /* tp_base */
    0,                                          /* tp_dict */
    0,                                          /* tp_descr_get */
    0,                                          /* tp_descr_set */
    0,                                          /* tp_dictoffset */
    0,                                          /* tp_init */
    0,                                          /* tp_alloc */
    0,                                          /* tp_new */
    0,                                          /* tp_free */
    0,                                          /* tp_is_gc */
    0,                                          /* tp_bases */
    0,                                          /* tp_mro */
    0,                                          /* tp_cache */
    0,                                          /* tp_subclasses */
    0,                                          /* tp_weaklist */
    0,                                          /* tp_del */
    0,                                          /* tp_version_tag */
};

/* End of code for ufunc objects */
