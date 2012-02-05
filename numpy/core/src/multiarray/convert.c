#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "structmember.h"

#define NPY_NO_DEPRECATED_API NPY_API_VERSION
#define _MULTIARRAYMODULE
#include "numpy/arrayobject.h"
#include "numpy/arrayscalars.h"

#include "npy_config.h"

#include "numpy/npy_3kcompat.h"

#include "arrayobject.h"
#include "mapping.h"
#include "lowlevel_strided_loops.h"
#include "scalartypes.h"

#include "convert.h"

/*
 * Converts a subarray of 'self' into lists, with starting data pointer
 * 'dataptr' and from dimension 'startdim' to the last dimension of 'self'.
 *
 * Returns a new reference.
 */
static PyObject *
recursive_tolist(PyArrayObject *self, char *dataptr, int startdim)
{
    npy_intp i, n, stride;
    PyObject *ret, *item;

    /* Base case */
    if (startdim >= PyArray_NDIM(self)) {
        return PyArray_DESCR(self)->f->getitem(dataptr,self);
    }

    n = PyArray_DIM(self, startdim);
    stride = PyArray_STRIDE(self, startdim);

    ret = PyList_New(n);
    if (ret == NULL) {
        return NULL;
    }

    for (i = 0; i < n; ++i) {
        item = recursive_tolist(self, dataptr, startdim+1);
        if (item == NULL) {
            Py_DECREF(ret);
            return NULL;
        }
        PyList_SET_ITEM(ret, i, item);

        dataptr += stride;
    }

    return ret;
}

/*NUMPY_API
 * To List
 */
NPY_NO_EXPORT PyObject *
PyArray_ToList(PyArrayObject *self)
{
    return recursive_tolist(self, PyArray_DATA(self), 0);
}

/* XXX: FIXME --- add ordering argument to
   Allow Fortran ordering on write
   This will need the addition of a Fortran-order iterator.
 */

/*NUMPY_API
  To File
*/
NPY_NO_EXPORT int
PyArray_ToFile(PyArrayObject *self, FILE *fp, char *sep, char *format)
{
    npy_intp size;
    npy_intp n, n2;
    size_t n3, n4;
    PyArrayIterObject *it;
    PyObject *obj, *strobj, *tupobj, *byteobj;

    n3 = (sep ? strlen((const char *)sep) : 0);
    if (n3 == 0) {
        /* binary data */
        if (PyDataType_FLAGCHK(PyArray_DESCR(self), NPY_LIST_PICKLE)) {
            PyErr_SetString(PyExc_ValueError, "cannot write " \
                    "object arrays to a file in "   \
                    "binary mode");
            return -1;
        }

        if (PyArray_ISCONTIGUOUS(self)) {
            size = PyArray_SIZE(self);
            NPY_BEGIN_ALLOW_THREADS;

#if defined (_MSC_VER) && defined(_WIN64)
            /* Workaround Win64 fwrite() bug. Ticket #1660 */
            {
                npy_intp maxsize = 2147483648 / PyArray_DESCR(self)->elsize;
                npy_intp chunksize;

                n = 0;
                while (size > 0) {
                    chunksize = (size > maxsize) ? maxsize : size;
                    n2 = fwrite((const void *)
                             ((char *)PyArray_DATA(self) + (n * PyArray_DESCR(self)->elsize)),
                             (size_t) PyArray_DESCR(self)->elsize,
                             (size_t) chunksize, fp);
                    if (n2 < chunksize) {
                        break;
                    }
                    n += n2;
                    size -= chunksize;
                }
                size = PyArray_SIZE(self);
            }
#else
            n = fwrite((const void *)PyArray_DATA(self),
                    (size_t) PyArray_DESCR(self)->elsize,
                    (size_t) size, fp);
#endif
            NPY_END_ALLOW_THREADS;
            if (n < size) {
                PyErr_Format(PyExc_ValueError,
                        "%ld requested and %ld written",
                        (long) size, (long) n);
                return -1;
            }
        }
        else {
            NPY_BEGIN_THREADS_DEF;

            it = (PyArrayIterObject *) PyArray_IterNew((PyObject *)self);
            NPY_BEGIN_THREADS;
            while (it->index < it->size) {
                if (fwrite((const void *)it->dataptr,
                            (size_t) PyArray_DESCR(self)->elsize,
                            1, fp) < 1) {
                    NPY_END_THREADS;
                    PyErr_Format(PyExc_IOError,
                            "problem writing element"\
                            " %"NPY_INTP_FMT" to file",
                            it->index);
                    Py_DECREF(it);
                    return -1;
                }
                PyArray_ITER_NEXT(it);
            }
            NPY_END_THREADS;
            Py_DECREF(it);
        }
    }
    else {
        /*
         * text data
         */

        it = (PyArrayIterObject *)
            PyArray_IterNew((PyObject *)self);
        n4 = (format ? strlen((const char *)format) : 0);
        while (it->index < it->size) {
            obj = PyArray_DESCR(self)->f->getitem(it->dataptr, self);
            if (obj == NULL) {
                Py_DECREF(it);
                return -1;
            }
            if (n4 == 0) {
                /*
                 * standard writing
                 */
                strobj = PyObject_Str(obj);
                Py_DECREF(obj);
                if (strobj == NULL) {
                    Py_DECREF(it);
                    return -1;
                }
            }
            else {
                /*
                 * use format string
                 */
                tupobj = PyTuple_New(1);
                if (tupobj == NULL) {
                    Py_DECREF(it);
                    return -1;
                }
                PyTuple_SET_ITEM(tupobj,0,obj);
                obj = PyUString_FromString((const char *)format);
                if (obj == NULL) {
                    Py_DECREF(tupobj);
                    Py_DECREF(it);
                    return -1;
                }
                strobj = PyUString_Format(obj, tupobj);
                Py_DECREF(obj);
                Py_DECREF(tupobj);
                if (strobj == NULL) {
                    Py_DECREF(it);
                    return -1;
                }
            }
#if defined(NPY_PY3K)
            byteobj = PyUnicode_AsASCIIString(strobj);
#else
            byteobj = strobj;
#endif
            NPY_BEGIN_ALLOW_THREADS;
            n2 = PyBytes_GET_SIZE(byteobj);
            n = fwrite(PyBytes_AS_STRING(byteobj), 1, n2, fp);
            NPY_END_ALLOW_THREADS;
#if defined(NPY_PY3K)
            Py_DECREF(byteobj);
#endif
            if (n < n2) {
                PyErr_Format(PyExc_IOError,
                        "problem writing element %"NPY_INTP_FMT\
                        " to file", it->index);
                Py_DECREF(strobj);
                Py_DECREF(it);
                return -1;
            }
            /* write separator for all but last one */
            if (it->index != it->size-1) {
                if (fwrite(sep, 1, n3, fp) < n3) {
                    PyErr_Format(PyExc_IOError,
                            "problem writing "\
                            "separator to file");
                    Py_DECREF(strobj);
                    Py_DECREF(it);
                    return -1;
                }
            }
            Py_DECREF(strobj);
            PyArray_ITER_NEXT(it);
        }
        Py_DECREF(it);
    }
    return 0;
}

/*NUMPY_API*/
NPY_NO_EXPORT PyObject *
PyArray_ToString(PyArrayObject *self, NPY_ORDER order)
{
    npy_intp numbytes;
    npy_intp i;
    char *dptr;
    int elsize;
    PyObject *ret;
    PyArrayIterObject *it;

    if (order == NPY_ANYORDER)
        order = PyArray_ISFORTRAN(self);

    /*        if (PyArray_TYPE(self) == NPY_OBJECT) {
              PyErr_SetString(PyExc_ValueError, "a string for the data" \
              "in an object array is not appropriate");
              return NULL;
              }
    */

    numbytes = PyArray_NBYTES(self);
    if ((PyArray_ISCONTIGUOUS(self) && (order == NPY_CORDER))
        || (PyArray_ISFORTRAN(self) && (order == NPY_FORTRANORDER))) {
        ret = PyBytes_FromStringAndSize(PyArray_DATA(self), (Py_ssize_t) numbytes);
    }
    else {
        PyObject *new;
        if (order == NPY_FORTRANORDER) {
            /* iterators are always in C-order */
            new = PyArray_Transpose(self, NULL);
            if (new == NULL) {
                return NULL;
            }
        }
        else {
            Py_INCREF(self);
            new = (PyObject *)self;
        }
        it = (PyArrayIterObject *)PyArray_IterNew(new);
        Py_DECREF(new);
        if (it == NULL) {
            return NULL;
        }
        ret = PyBytes_FromStringAndSize(NULL, (Py_ssize_t) numbytes);
        if (ret == NULL) {
            Py_DECREF(it);
            return NULL;
        }
        dptr = PyBytes_AS_STRING(ret);
        i = it->size;
        elsize = PyArray_DESCR(self)->elsize;
        while (i--) {
            memcpy(dptr, it->dataptr, elsize);
            dptr += elsize;
            PyArray_ITER_NEXT(it);
        }
        Py_DECREF(it);
    }
    return ret;
}

/*NUMPY_API*/
NPY_NO_EXPORT int
PyArray_FillWithScalar(PyArrayObject *arr, PyObject *obj)
{
    PyArray_Descr *dtype = NULL;
    npy_longlong value_buffer[4];
    char *value = NULL;
    int retcode = 0;

    /*
     * If 'arr' is an object array, copy the object as is unless
     * 'obj' is a zero-dimensional array, in which case we copy
     * the element in that array instead.
     */
    if (PyArray_DESCR(arr)->type_num == NPY_OBJECT &&
                        !(PyArray_Check(obj) &&
                          PyArray_NDIM((PyArrayObject *)obj) == 0)) {
        value = (char *)&obj;

        dtype = PyArray_DescrFromType(NPY_OBJECT);
        if (dtype == NULL) {
            return -1;
        }
    }
    /* NumPy scalar */
    else if (PyArray_IsScalar(obj, Generic)) {
        dtype = PyArray_DescrFromScalar(obj);
        if (dtype == NULL) {
            return -1;
        }
        value = scalar_value(obj, dtype);
        if (value == NULL) {
            Py_DECREF(dtype);
            return -1;
        }
    }
    /* Python boolean */
    else if (PyBool_Check(obj)) {
        value = (char *)value_buffer;
        *value = (obj == Py_True);

        dtype = PyArray_DescrFromType(NPY_BOOL);
        if (dtype == NULL) {
            return -1;
        }
    }
    /* Python integer */
    else if (PyLong_Check(obj) || PyInt_Check(obj)) {
        npy_longlong v = PyLong_AsLongLong(obj);
        if (v == -1 && PyErr_Occurred()) {
            return -1;
        }
        value = (char *)value_buffer;
        *(npy_longlong *)value = v;

        dtype = PyArray_DescrFromType(NPY_LONGLONG);
        if (dtype == NULL) {
            return -1;
        }
    }
    /* Python float */
    else if (PyFloat_Check(obj)) {
        npy_double v = PyFloat_AsDouble(obj);
        if (v == -1 && PyErr_Occurred()) {
            return -1;
        }
        value = (char *)value_buffer;
        *(npy_double *)value = v;

        dtype = PyArray_DescrFromType(NPY_DOUBLE);
        if (dtype == NULL) {
            return -1;
        }
    }
    /* Python complex */
    else if (PyComplex_Check(obj)) {
        npy_double re, im;

        re = PyComplex_RealAsDouble(obj);
        if (re == -1 && PyErr_Occurred()) {
            return -1;
        }
        im = PyComplex_ImagAsDouble(obj);
        if (im == -1 && PyErr_Occurred()) {
            return -1;
        }
        value = (char *)value_buffer;
        ((npy_double *)value)[0] = re;
        ((npy_double *)value)[1] = im;

        dtype = PyArray_DescrFromType(NPY_CDOUBLE);
        if (dtype == NULL) {
            return -1;
        }
    }

    /* Use the value pointer we got if possible */
    if (value != NULL) {
        /* TODO: switch to SAME_KIND casting */
        retcode = PyArray_AssignRawScalar(arr, dtype, value,
                                NULL, NPY_UNSAFE_CASTING, 0, NULL);
        Py_DECREF(dtype);
        return retcode;
    }
    /* Otherwise convert to an array to do the assignment */
    else {
        PyArrayObject *src_arr;

        src_arr = (PyArrayObject *)PyArray_FromAny(obj, NULL, 0, 0,
                                            NPY_ARRAY_ALLOWNA, NULL);
        if (src_arr == NULL) {
            return -1;
        }

        if (PyArray_NDIM(src_arr) != 0) {
            PyErr_SetString(PyExc_ValueError,
                    "Input object to FillWithScalar is not a scalar");
            Py_DECREF(src_arr);
            return -1;
        }

        retcode = PyArray_CopyInto(arr, src_arr);

        Py_DECREF(src_arr);
        return retcode;
    }
}

/*NUMPY_API
 *
 * Fills an array with zeros.
 *
 * dst: The destination array.
 * wheremask: If non-NULL, a boolean mask specifying where to set the values.
 * preservena: If 0, overwrites everything in 'dst', if 1, it
 *              preserves elements in 'dst' which are NA.
 * preservewhichna: Must be NULL. When multi-NA support is implemented,
 *                   this will be an array of flags for 'preservena=True',
 *                   indicating which NA payload values to preserve.
 *
 * Returns 0 on success, -1 on failure.
 */
NPY_NO_EXPORT int
PyArray_AssignZero(PyArrayObject *dst,
                    PyArrayObject *wheremask,
                    npy_bool preservena, npy_bool *preservewhichna)
{
    npy_bool value;
    PyArray_Descr *bool_dtype;
    int retcode;

    /* Create a raw bool scalar with the value False */
    bool_dtype = PyArray_DescrFromType(NPY_BOOL);
    if (bool_dtype == NULL) {
        return -1;
    }
    value = 0;

    retcode = PyArray_AssignRawScalar(dst, bool_dtype, (char *)&value,
                                wheremask, NPY_SAFE_CASTING,
                                preservena, preservewhichna);

    Py_DECREF(bool_dtype);
    return retcode;
}

/*NUMPY_API
 *
 * Fills an array with ones.
 *
 * dst: The destination array.
 * wheremask: If non-NULL, a boolean mask specifying where to set the values.
 * preservena: If 0, overwrites everything in 'dst', if 1, it
 *              preserves elements in 'dst' which are NA.
 * preservewhichna: Must be NULL. When multi-NA support is implemented,
 *                   this will be an array of flags for 'preservena=True',
 *                   indicating which NA payload values to preserve.
 *
 * Returns 0 on success, -1 on failure.
 */
NPY_NO_EXPORT int
PyArray_AssignOne(PyArrayObject *dst,
                    PyArrayObject *wheremask,
                    npy_bool preservena, npy_bool *preservewhichna)
{
    npy_bool value;
    PyArray_Descr *bool_dtype;
    int retcode;

    /* Create a raw bool scalar with the value True */
    bool_dtype = PyArray_DescrFromType(NPY_BOOL);
    if (bool_dtype == NULL) {
        return -1;
    }
    value = 1;

    retcode = PyArray_AssignRawScalar(dst, bool_dtype, (char *)&value,
                                wheremask, NPY_SAFE_CASTING,
                                preservena, preservewhichna);

    Py_DECREF(bool_dtype);
    return retcode;
}

/*NUMPY_API
 * Copy an array.
 */
NPY_NO_EXPORT PyObject *
PyArray_NewCopy(PyArrayObject *obj, NPY_ORDER order)
{
    PyArrayObject *ret;

    ret = (PyArrayObject *)PyArray_NewLikeArray(obj, order, NULL, 1);
    if (ret == NULL) {
        return NULL;
    }

    if (PyArray_HASMASKNA(obj)) {
        if (PyArray_AllocateMaskNA(ret, 1, 0, 1) < 0) {
            Py_DECREF(ret);
            return NULL;
        }
    }

    if (PyArray_AssignArray(ret, obj, NULL, NPY_UNSAFE_CASTING, 0, NULL) < 0) {
        Py_DECREF(ret);
        return NULL;
    }

    return (PyObject *)ret;
}

/*NUMPY_API
 * View
 * steals a reference to type -- accepts NULL
 */
NPY_NO_EXPORT PyObject *
PyArray_View(PyArrayObject *self, PyArray_Descr *type, PyTypeObject *pytype)
{
    PyArrayObject *ret = NULL;
    PyArray_Descr *dtype;
    PyTypeObject *subtype;
    int flags;

    if (pytype) {
        subtype = pytype;
    }
    else {
        subtype = Py_TYPE(self);
    }

    flags = PyArray_FLAGS(self);
    flags &= ~(NPY_ARRAY_MASKNA|NPY_ARRAY_OWNMASKNA);

    dtype = PyArray_DESCR(self);
    Py_INCREF(dtype);
    ret = (PyArrayObject *)PyArray_NewFromDescr(subtype,
                               dtype,
                               PyArray_NDIM(self), PyArray_DIMS(self),
                               PyArray_STRIDES(self),
                               PyArray_DATA(self),
                               flags,
                               (PyObject *)self);
    if (ret == NULL) {
        return NULL;
    }

    /* Take a view of the mask if it exists */
    if (PyArray_HASMASKNA(self)) {
        PyArrayObject_fields *fa = (PyArrayObject_fields *)ret;

        if (PyArray_HASFIELDS(self)) {
            PyErr_SetString(PyExc_RuntimeError,
                    "NA masks with fields are not supported yet");
            Py_DECREF(ret);
            Py_DECREF(type);
            return NULL;
        }

        fa->maskna_dtype = PyArray_MASKNA_DTYPE(self);
        Py_INCREF(fa->maskna_dtype);
        fa->maskna_data = PyArray_MASKNA_DATA(self);
        if (fa->nd > 0) {
            memcpy(fa->maskna_strides, PyArray_MASKNA_STRIDES(self),
                                            fa->nd * sizeof(npy_intp));
        }
        fa->flags |= NPY_ARRAY_MASKNA;
    }

    /* Set the base object */
    Py_INCREF(self);
    if (PyArray_SetBaseObject(ret, (PyObject *)self) < 0) {
        Py_DECREF(ret);
        Py_DECREF(type);
        return NULL;
    }

    if (type != NULL) {
        if (PyObject_SetAttrString((PyObject *)ret, "dtype",
                                   (PyObject *)type) < 0) {
            Py_DECREF(ret);
            Py_DECREF(type);
            return NULL;
        }
        Py_DECREF(type);
    }
    return (PyObject *)ret;
}
