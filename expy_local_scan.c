/*
 * Python wrapper for Exim local_scan feature
 *
 * 2002-10-20  Barry Pederson <bp@barryp.org>
 *
 */
#include <errno.h>

#include <Python.h>
#include "local_scan.h"

/* ---- Settings controllable at runtime through Exim 'configure' file --------

 This local_scan module will act *somewhat* like this python-ish pseudocode:

   try:
       if {expy_path_add}:
           import sys
           sys.path.append({expy_path_add})

       import {expy_scan_module}

       rc = {expy_scan_module}.{expy_scan_function}()

       if rc is sequence:
           if len(rc) > 1:
               return_text = str(rc[1])
           rc = rc[0]

       assert rc is integer
       return rc
   except:
       return_text = "some description of problem"
       return PYTHON_FAILURE_RETURN

 And a do-nothing {expy_scan_module}.py file might look like:

   import {expy_exim_module}

   def {expy_scan_function}():
       return {expy_exim_module}.LOCAL_SCAN_ACCEPT

*/

static BOOL    expy_enabled = TRUE;
static uschar *expy_path_add = NULL;
static uschar *expy_exim_module = US"exim";
static uschar *expy_scan_module = US"exim_local_scan";
static uschar *expy_scan_function = US"local_scan";
static uschar *expy_scan_failure = US"defer";

optionlist local_scan_options[] =
    {
    { "expy_enabled", opt_bool, &expy_enabled},
    { "expy_exim_module",  opt_stringptr, &expy_exim_module },
    { "expy_path_add",  opt_stringptr, &expy_path_add },
    { "expy_scan_failure",  opt_stringptr, &expy_scan_failure},
    { "expy_scan_function",  opt_stringptr, &expy_scan_function },
    { "expy_scan_module",  opt_stringptr, &expy_scan_module },
    };

int local_scan_options_count = sizeof(local_scan_options)/sizeof(optionlist);

/* ------- Private Globals ------------ */

static PyObject *expy_exim_dict = NULL;
static PyObject *expy_user_module = NULL;


/* ------- Custom type for holding header lines ------

  Basically an object with .text and .type attributes, only the
  .type attribute is changable, and only to single-character
  values.  Usually it'd be '*' which Exim interprets as
  meaning the line should be deleted.

  Also accessible for backwards compatibility as a sequence

*/

typedef struct
    {
    PyObject_HEAD
    header_line *hline;
    } expy_header_line_t;


static void expy_header_line_dealloc(PyObject *self)
    {
    PyObject_Del(self);
    }


static PyObject * expy_header_line_getattr(expy_header_line_t *self, char *name)
    {
    if (self->hline == NULL)
        {
        PyErr_Format(PyExc_AttributeError, "Header object no longer valid, held over from previously processed message?");
        return NULL;
        }

    if (!strcmp(name, "text"))
        return PyString_FromString((const char *)self->hline->text);

    if (!strcmp(name, "type"))
        {
        char ch = (char)(self->hline->type);
        return PyString_FromStringAndSize(&ch, 1);
        }

    PyErr_Format(PyExc_AttributeError, "Unknown attribute: %s", name);
    return NULL;
    }


static int expy_header_line_setattr(expy_header_line_t *self, char *name, PyObject *value)
    {
    if (self->hline == NULL)
        {
        PyErr_Format(PyExc_AttributeError, "Header object no longer valid, held over from previously processed message?");
        return -1;
        }

    if (!strcmp(name, "type"))
        {
        char *p;
#if PY_MINOR_VERSION < 5
        int len;
#else
        Py_ssize_t len;
#endif

        if (PyString_AsStringAndSize(value, &p, &len) == -1)
            return -1;

        if (len != 1)
            {
            PyErr_SetString(PyExc_TypeError, "header.type can only be set to a single-character value");
            return -1;
            }

        self->hline->type = (int)(p[0]);
        return 0;
        }

    PyErr_Format(PyExc_AttributeError, "Attribute: %s is not settable", name);
    return -1;
    }


static PyTypeObject ExPy_Header_Line  =
    {
    PyObject_HEAD_INIT(NULL)    /* Workaround problem with Cygwin/GCC, by setting to &PyType_Type at runtime */
    0,                          /*ob_size*/
    "ExPy Header Line",         /*tp_name*/
    sizeof(expy_header_line_t), /*tp_size*/
    0,                          /*tp_itemsize*/
    expy_header_line_dealloc,   /*tp_dealloc*/
    0,                          /*tp_print*/
    (getattrfunc) expy_header_line_getattr,  /*tp_getattr*/
    (setattrfunc) expy_header_line_setattr,  /*tp_setattr*/
    };


PyObject * expy_create_header_line(header_line *p)
    {
    expy_header_line_t * result;

    result = PyObject_NEW(expy_header_line_t, &ExPy_Header_Line);  /* New Reference */
    if (!result)
        return NULL;

    result->hline = p;

    return (PyObject *) result;
    }


/* ------- Helper functions for Module methods ------- */

/*
 * Given a C string, make sure it's safe to pass to a
 * printf-style function ('%' chars are escaped by doubling
 * them up.  Optionally, also make sure the string ends with a '\n'
 */
static char *get_format_string(char *str, int need_newline)
    {
    char *p;
    char *q;
    char *newstr;
    /*
     * If there are more percentage signs, or if the string is longer,
     * than the maximum number that will fit in an unsigned int on this
     * platform, then this will overflow.  That seems extremely
     * unlikely.
     */
    unsigned int percent_count;
    unsigned int len;
    /* Count number of '%' characters in string, and get the total length while at it */
    for (p = str, percent_count = 0; *p; p++)
        if (*p == '%')
            percent_count++;
    len = p - str;

    /* Decide if we need a newline added */
    if (need_newline)
        {
        if (len && (*(p-1) == '\n'))
            need_newline = 0;
        else
            need_newline = 1; /* paranoia - just in case something other than 1 was used to indicate truth */
        }

    /* If it's all good, just return the string we were passed */
    if ((!percent_count) && (!need_newline))
        return str;

    /* Gotta make a new string, with '%'s and/or '\n' added */
    newstr = store_get(len + percent_count + need_newline + 1, 0);

    for (p = str, q = newstr; *p; p++, q++)
        {
        *q = *p;
        if (*q == '%')
            {
            q++;
            *q = '%';
            }
        }

    if (need_newline)
        {
        *q = '\n';
        q++;
        }

    *q = 0;

    return newstr;
    }

/* -------- Module Methods ------------ */

/*
 * Have Exim do a string expansion, will raise
 * a Python ValueError exception if the expansion fails
 */
static PyObject *expy_expand_string(PyObject *self, PyObject *args)
    {
    char *str;
    uschar *result;

    if (!PyArg_ParseTuple(args, "s", &str))
        return NULL;

    result = expand_string((uschar *)str);

    if (!result)
        {
        PyErr_Format(PyExc_ValueError, "expansion [%s] failed: %s", str, expand_string_message);
        return NULL;
        }

    return PyString_FromString((const char *)result);
    }


/*
 * Add a header line, will automatically tack on a '\n' if necessary
 */
static PyObject *expy_header_add(PyObject *self, PyObject *args)
    {
    char *str;

    if (!PyArg_ParseTuple(args, "s", &str))
        return NULL;

    header_add(' ', get_format_string(str, 1));
    PyList_Append(PyDict_GetItemString(expy_exim_dict, "headers"),
                  expy_create_header_line(header_last));

    Py_INCREF(Py_None);
    return Py_None;
    }


/*
 * Write to exim log, uses LOG_MAIN by default
 */
static PyObject *expy_log_write(PyObject *self, PyObject *args)
    {
    char *str;
    int which = LOG_MAIN;

    if (!PyArg_ParseTuple(args, "s|i", &str, &which))
        return NULL;

    log_write(0, which, "%s", get_format_string(str, 0));

    Py_INCREF(Py_None);
    return Py_None;
    }

/*
 * Print through Exim's debug_print() function, which does nothing if
 * Exim isn't in debugging mode.
 */
static PyObject *expy_debug_print(PyObject *self, PyObject *args)
    {
    char *str;

    if (!PyArg_ParseTuple(args, "s", &str))
        return NULL;

    debug_printf("%s", get_format_string(str, 0));

    Py_INCREF(Py_None);
    return Py_None;
    }

/*
 * Create a child process that runs the command specified.
 * The return value is (stdout, stdin, pid), where stdout and stdin are file
 * descriptors to the appropriate pipes (stderr is joined with stdout).
 * The environment may be specified, and a new umask supplied.
 */
static PyObject *expy_child_open(PyObject *self, PyObject *args)
    {
    pid_t pid;
    int infdptr;
    int outfdptr;
    PyObject * py_argv;
    PyObject * py_envp;
    int umask;
    unsigned char make_leader = 0;
    Py_ssize_t i;
    Py_ssize_t argc;
    uschar ** argv;
    uschar ** envp;
    Py_ssize_t envp_len;

    /*
     * The first two arguments are tuples of strings (argv, envp).
     */
    if (!PyArg_ParseTuple(args, "OOi|b", &py_argv, &py_envp, &umask, &make_leader))
        return NULL;

    argc = PySequence_Size(py_argv);
    argv = PyMem_New(uschar *, argc + 1);
    for (i=0; i<argc; ++i)
        {
        argv[i] = (uschar *)PyString_AsString(PyTuple_GET_ITEM(py_argv, i)); /* borrowed ref */
        }
    argv[argc] = NULL;
    envp_len = PySequence_Size(py_envp);
    envp = PyMem_New(uschar *, envp_len + 1);
    for (i=0; i<envp_len; ++i)
        {
        envp[i] = (uschar *)PyString_AsString(PyTuple_GET_ITEM(py_envp, i)); /* borrowed ref */
        }
    envp[envp_len] = NULL;
    pid = child_open(argv, envp, umask, &infdptr, &outfdptr, (BOOL) make_leader);
    PyMem_Del(argv);
    PyMem_Del(envp);
    if (pid == -1)
    {
        /*
         * An error occurred.
         */
        PyErr_Format(PyExc_OSError, "error %d", errno);
        return NULL;
    }

    return Py_BuildValue("(iii)", infdptr, outfdptr, pid);
    }

/*
 * Wait for a child process to terminate, or for a timeout (in seconds) to
 * expire.  A timeout of zero (the default) means wait as long as it takes.
 * The return value is the process ending status.
 */
static PyObject *expy_child_close(PyObject *self, PyObject *args)
    {
    int pid;
    int timeout = 0;
    int result;

    if (!PyArg_ParseTuple(args, "i|i", &pid, &timeout))
        return NULL;

    result = child_close((pid_t) pid, timeout);

    if (result < 0 && result > -256)
    {
        /*
         * The process was ended by a signal.  The result is the negation
         * of the signal number.
         */
        PyErr_Format(PyExc_OSError, "ended by signal %d", result * -1);
        return NULL;
    }
    else if (result == -256)
    {
        /*
         * The process timed out.
         */
        PyErr_Format(PyExc_OSError, "timed out");
        return NULL;
    }
    else if (result < -256)
    {
        /*
         * An error occurred.
         */
        PyErr_Format(PyExc_OSError, "error %d", errno);
        return NULL;
    }

    return PyInt_FromLong(result);
    }

/*
 * Note that this is the child_open_exim2 method from the Exim local_scan
 * API - any child_open_exim call can be done through this method as well.
 * Also, rather than returning a file descriptor, we take the message
 * content as an argument, and write it out to the subprocess.  We still
 * return the PID, so that execution can continue while Exim is processing
 * the message if the caller so desires.
 * Submit a new message to Exim, returning the PID of the subprocess.
 * Essentially, this is running 'exim -t -oem -oi -f sender -oMas auth'
 * (-oMas is omitted if no authentication is provided).
 */
static PyObject *expy_child_open_exim(PyObject *self, PyObject *args)
    {
    char *message;
    int message_length;
    char *sender = "";
    char *sender_authentication = NULL;
    pid_t exim_pid;
    int fd;

    if (!PyArg_ParseTuple(args, "s#|ss", &message, &message_length, &sender, &sender_authentication))
        return NULL;

    exim_pid = child_open_exim2(&fd, (uschar *) sender, (uschar *) sender_authentication);
    if (write(fd, message, message_length) <= 0)
    {
        /*
         * An error occurred.
         */
        PyErr_Format(PyExc_OSError, "error %d", errno);
	close(fd);
        return NULL;
    }
    close(fd);
    return PyInt_FromLong(exim_pid);
    }


static PyMethodDef expy_exim_methods[] =
    {
    {"expand", expy_expand_string, METH_VARARGS, "Have exim expand string."},
    {"log", expy_log_write, METH_VARARGS, "Write message to exim log."},
    {"add_header", expy_header_add, METH_VARARGS, "Add header to message."},
    {"debug_print", expy_debug_print, METH_VARARGS, "Print if Exim is in debugging mode, otherwise do nothing."},
    {"child_open", expy_child_open, METH_VARARGS, "Create a child process."},
    {"child_close", expy_child_close, METH_VARARGS, "Wait for a child process to terminate."},
    {"child_open_exim", expy_child_open_exim, METH_VARARGS, "Submit a message to Exim."},
    {NULL, NULL, 0, NULL}
    };


/* ------------  Helper Functions for local_scan ---------- */

/*
 * Add a string to the module dictionary
 */
static void expy_dict_string(char *key, uschar *val)
    {
    PyObject *s;

    if (val)
        s = PyString_FromString((const char *)val);
    else
        {
        s = Py_None;
        Py_INCREF(s);
        }

    PyDict_SetItemString(expy_exim_dict, key, s);
    Py_DECREF(s);
    }

/*
 * Add an integer to the module dictionary
 */
static void expy_dict_int(char *key, int val)
    {
    PyObject *i;

    i = PyInt_FromLong(val);
    PyDict_SetItemString(expy_exim_dict, key, i);
    Py_DECREF(i);
    }

/*
 * Convert Exim header linked-list to Python list
 * of header objects.
 *
 * Returns New reference
 */
static PyObject *get_headers()
    {
    header_line *p;
    PyObject *result;

    /* Build up the list of tuples */
    result = PyList_New(0);           /* New reference */
    for (p = header_list; p; p = p->next)
        {
        PyList_Append(result, expy_create_header_line(p));   /* Steals new reference */
        }

    return result;
    }

/*
 * Given the header tuple created by get_headers(), go through
 * and set the header objects to point to NULL, in case someone
 * tries to re-use them after a message is done being processed, and
 * the underlying header strings are no longer available
 */
static void clear_headers(PyObject *exim_headers)
    {
    int i, n;

    n = PyList_Size(exim_headers);
    for (i = 0; i < n; i++)
        {
        expy_header_line_t *p;

        p = (expy_header_line_t *) PyList_GetItem(exim_headers, i); /* Borrowed reference */
        p->hline = NULL;
        }

    }


/*
 * Make tuple containing message recipients
 */
static PyObject *get_recipients()
    {
    PyObject *result;
    Py_ssize_t i;

    result = PyTuple_New(recipients_count);
    for (i = 0; i < recipients_count; i++)
        PyTuple_SetItem(result, i, PyString_FromString((const char *)recipients_list[i].address));

    return result;
    }

/*
 * shift entries in list down to overwrite
 * entry in slot n (0-based)
 */
static void expy_remove_recipient(int n)
    {
    int i;
    for (i = n; i < (recipients_count-1); i ++)
        recipients_list[i] = recipients_list[i+1];

    recipients_count--;
    }


/* ----------- Actual local_scan function ------------ */

char* getPythonTraceback()
{
    /* Python equivalent:

       import traceback, sys
       return "".join(traceback.format_exception(sys.exc_type,
                                                 sys.exc_value,
                                                 sys.exc_traceback))
    */

    PyObject *type, *value, *traceback;
    PyObject *tracebackModule;
    char *chrRetval;

    PyErr_Fetch(&type, &value, &traceback);

    tracebackModule = PyImport_ImportModule("traceback");
    if (tracebackModule != NULL)
    {
        PyObject *tbList, *emptyString, *strRetval;

        tbList = PyObject_CallMethod(
            tracebackModule,
            "format_exception",
            "OOO",
            type,
            value == NULL ? Py_None : value,
            traceback == NULL ? Py_None : traceback);

        emptyString = PyString_FromString("");
        strRetval = PyObject_CallMethod(emptyString, "join",
            "O", tbList);

        chrRetval = strdup(PyString_AsString(strRetval));

        Py_DECREF(tbList);
        Py_DECREF(emptyString);
        Py_DECREF(strRetval);
        Py_DECREF(tracebackModule);
    }
    else
    {
        chrRetval = strdup("Unable to import traceback module.");
    }

    Py_DECREF(type);
    Py_XDECREF(value);
    Py_XDECREF(traceback);

    return chrRetval;
}


int local_scan(int fd, uschar **return_text)
    {
    int python_failure_return = LOCAL_SCAN_TEMPREJECT;
    PyObject *user_dict;
    PyObject *user_func;
    PyObject *result;
    PyObject *exim_headers;
    PyObject *original_recipients;
    PyObject *working_recipients;

    if (!expy_enabled)
        return LOCAL_SCAN_ACCEPT;

    if (strcmpic(expy_scan_failure, US"accept") == 0)
        python_failure_return = LOCAL_SCAN_ACCEPT;
    else if (strcmpic(expy_scan_failure, US"defer") == 0)
        python_failure_return = LOCAL_SCAN_TEMPREJECT;
    else if (strcmpic(expy_scan_failure, US"deny") == 0)
        python_failure_return = LOCAL_SCAN_REJECT;

    if (!Py_IsInitialized())  /* local_scan() may have already been run */
        {
        /* It is definitely cleanest to set a program name here. 
        However, it's not really clear *what* name to use. In many ways,
        Exim would be most accurate, but that will not necessarily be the
        starting location for finding libraries that is wanted.
        Hard-coding /usr/local/ here is SE specific, and something more
        generic would need to be used to submit this upstream. */
        Py_SetProgramName("/usr/local/bin/python");
        Py_Initialize();
        ExPy_Header_Line.ob_type = &PyType_Type;
        }

    if (!expy_exim_dict)
        {
        PyObject *module = Py_InitModule((const char *)expy_exim_module, expy_exim_methods); /* Borrowed reference */
        Py_INCREF(module);                                 /* convert to New reference */
        expy_exim_dict = PyModule_GetDict(module);         /* Borrowed reference */
        Py_INCREF(expy_exim_dict);                         /* convert to New reference */
        }

    if (!expy_user_module)
        {
        if (expy_path_add)
            {
            PyObject *sys_module;
            PyObject *sys_dict;
            PyObject *sys_path;
            PyObject *add_value;

            sys_module = PyImport_ImportModule("sys");  /* New Reference */
            if (!sys_module)
                {
                *return_text = (uschar *)"Internal error";
                log_write(0, LOG_PANIC, "Couldn't import Python 'sys' module");
                log_write(0, LOG_PANIC, "%s", getPythonTraceback());
                return python_failure_return;
                }

            sys_dict = PyModule_GetDict(sys_module);               /* Borrowed Reference, never fails */
            sys_path = PyMapping_GetItemString(sys_dict, "path");  /* New reference */

            if (!sys_path || (!PyList_Check(sys_path)))
                {
                *return_text = (uschar *)"Internal error";
                log_write(0, LOG_PANIC, "expy: Python sys.path doesn't exist or isn't a list");
                log_write(0, LOG_PANIC, "%s", getPythonTraceback());
                return python_failure_return;
                }

            add_value = PyString_FromString((const char *)expy_path_add);  /* New reference */
            if (!add_value)
                {
                PyErr_Clear();
                log_write(0, LOG_PANIC, "expy: Failed to create Python string from [%s]", expy_path_add);
                return python_failure_return;
                }

            if (PyList_Append(sys_path, add_value))
                {
                PyErr_Clear();
                log_write(0, LOG_PANIC, "expy: Failed to append [%s] to Python sys.path", expy_path_add);
                }

            Py_DECREF(add_value);
            Py_DECREF(sys_path);
            Py_DECREF(sys_module);
            }

        expy_user_module = PyImport_ImportModule((const char *)expy_scan_module);  /* New Reference */

        if (!expy_user_module)
            {
            PyErr_Clear();
            *return_text = (uschar *)"Internal error";
            log_write(0, LOG_PANIC, "Couldn't import Python '%s' module", expy_scan_module);
            return python_failure_return;
            }
        }

    user_dict = PyModule_GetDict(expy_user_module);                      /* Borrowed Reference, never fails */
    user_func = PyMapping_GetItemString(user_dict, (char *)expy_scan_function);  /* New reference */

    if (!user_func)
        {
        PyErr_Clear();
        *return_text = (uschar *)"Internal error";
        log_write(0, LOG_PANIC, "Python %s module doesn't have a %s function", expy_scan_module, expy_scan_function);
        return python_failure_return;
        }

    /* so far so good, prepare to run function */

    /* Copy exim variables */
    expy_dict_int("debug_selector", debug_selector);
    expy_dict_int("host_checking", host_checking);
    expy_dict_string("interface_address", interface_address);
    expy_dict_int("interface_port", interface_port);
    expy_dict_string("message_id", message_id);
    expy_dict_string("received_protocol", received_protocol);
    expy_dict_string("sender_address", sender_address);
    expy_dict_string("sender_host_address", sender_host_address);
    expy_dict_string("sender_host_authenticated", sender_host_authenticated);
    expy_dict_string("sender_host_name", sender_host_name);
    expy_dict_int("sender_host_port", sender_host_port);
    expy_dict_int("fd", fd);

    /* copy some constants */
    expy_dict_int("LOG_MAIN", LOG_MAIN);
    expy_dict_int("LOG_PANIC", LOG_PANIC);
    expy_dict_int("LOG_REJECT", LOG_REJECT);

    expy_dict_int("LOCAL_SCAN_ACCEPT", LOCAL_SCAN_ACCEPT);
    expy_dict_int("LOCAL_SCAN_ACCEPT_FREEZE", LOCAL_SCAN_ACCEPT_FREEZE);
    expy_dict_int("LOCAL_SCAN_ACCEPT_QUEUE", LOCAL_SCAN_ACCEPT_QUEUE);
    expy_dict_int("LOCAL_SCAN_REJECT", LOCAL_SCAN_REJECT);
    expy_dict_int("LOCAL_SCAN_REJECT_NOLOGHDR", LOCAL_SCAN_REJECT_NOLOGHDR);
    expy_dict_int("LOCAL_SCAN_TEMPREJECT", LOCAL_SCAN_TEMPREJECT);
    expy_dict_int("LOCAL_SCAN_TEMPREJECT_NOLOGHDR", LOCAL_SCAN_TEMPREJECT_NOLOGHDR);
    expy_dict_int("MESSAGE_ID_LENGTH", MESSAGE_ID_LENGTH);
    expy_dict_int("SPOOL_DATA_START_OFFSET", SPOOL_DATA_START_OFFSET);

    expy_dict_int("D_v", D_v);
    expy_dict_int("D_local_scan", D_local_scan);

    /* set the headers */
    exim_headers = get_headers();
    PyDict_SetItemString(expy_exim_dict, "headers", exim_headers);

    /*
     * make list of recipients, give module a copy to work with in
     * List format, but keep original tuple to compare against later
     */
    original_recipients = get_recipients();                     /* New reference */
    working_recipients = PySequence_List(original_recipients);  /* New reference */
    PyDict_SetItemString(expy_exim_dict, "recipients", working_recipients);
    Py_DECREF(working_recipients);

    /* Try calling our function */
    result = PyObject_CallFunction(user_func, NULL);            /* New reference */

    Py_DECREF(user_func);  /* Don't need ref to function anymore */

    /* Check for Python exception */
    if (!result)
        {
        *return_text = (uschar *)"Internal error";
        log_write(0, LOG_PANIC, "local_scan function failed");
        log_write(0, LOG_PANIC, "%s", getPythonTraceback());
        Py_DECREF(original_recipients);
        clear_headers(exim_headers);
        Py_DECREF(exim_headers);
        return python_failure_return;
        }

    /* User code may have replaced recipient list, so re-get ref */
    working_recipients = PyDict_GetItemString(expy_exim_dict, "recipients"); /* Borrowed reference */
    Py_XINCREF(working_recipients);                                           /* convert to New reference */

    /*
     * reconcile original recipient list with what's present after
     * Python code is done
     */
    if ((!working_recipients) || (!PySequence_Check(working_recipients)) || (PySequence_Size(working_recipients) == 0))
        /* Python code either deleted exim.recipients altogether, or replaced
           it with a non-list, or emptied out the list */
        recipients_count = 0;
    else
        {
        Py_ssize_t i;

        /* remove original recipients not on the working list, reverse order important! */
        for (i = recipients_count - 1; i >= 0; i--)
            {
            PyObject *addr = PyTuple_GET_ITEM(original_recipients, i); /* borrowed ref */
            if (!PySequence_Contains(working_recipients, addr))
                expy_remove_recipient(i);
            }

        /* add new recipients not in the original list */
        for (i = PySequence_Size(working_recipients) - 1; i >= 0; i--)
            {
            PyObject *addr = PySequence_GetItem(working_recipients, i);
            if (!PySequence_Contains(original_recipients, addr))
                receive_add_recipient((uschar *)PyString_AsString(addr), -1);
            Py_DECREF(addr);
            }
        }

    Py_XDECREF(working_recipients);   /* No longer needed */
    Py_DECREF(original_recipients);   /* No longer needed */

    clear_headers(exim_headers);
    Py_DECREF(exim_headers);          /* No longer needed */

    /* Deal with the return value, first see if python returned a non-empty sequence */
    if (PySequence_Check(result) && (PySequence_Size(result) > 0))
        {
        /* save first item */
        PyObject *rc = PySequence_GetItem(result, 0);

        /* if more than one item, convert 2nd item to string and use as return text */
        if (PySequence_Size(result) > 1)
            {
            PyObject *str;
            PyObject *obj = PySequence_GetItem(result, 1);   /* New reference */
            str = PyObject_Str(obj);                         /* New reference */

            *return_text = string_copy((uschar *)PyString_AsString(str));

            Py_DECREF(obj);
            Py_DECREF(str);
            }

        /* drop the sequence, and focus on the first item we saved */
        Py_DECREF(result);
        result = rc;
        }

    /* If we have an integer, return that to Exim */
    if (PyInt_Check(result))
        {
        int rc = PyInt_AsLong(result);
        Py_DECREF(result);
        return rc;
        }

    /* didn't return anything usable */
    Py_DECREF(result);
    *return_text = (uschar *)"Internal error";
    log_write(0, LOG_PANIC, "Python %s.%s function didn't return integer", expy_scan_module, expy_scan_function);
    return python_failure_return;
    }


/*-------- EOF --------------*/
