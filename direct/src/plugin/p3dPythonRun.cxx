// Filename: p3dPythonRun.cxx
// Created by:  drose (05Jun09)
//
////////////////////////////////////////////////////////////////////
//
// PANDA 3D SOFTWARE
// Copyright (c) Carnegie Mellon University.  All rights reserved.
//
// All use of this software is subject to the terms of the revised BSD
// license.  You should have received a copy of this license along
// with this source code in a file named "LICENSE."
//
////////////////////////////////////////////////////////////////////

#include "p3dPythonRun.h"
#include "asyncTaskManager.h"

// There is only one P3DPythonRun object in any given process space.
// Makes the statics easier to deal with, and we don't need multiple
// instances of this think.
P3DPythonRun *P3DPythonRun::_global_ptr = NULL;

////////////////////////////////////////////////////////////////////
//     Function: P3DPythonRun::Constructor
//       Access: Public
//  Description: 
////////////////////////////////////////////////////////////////////
P3DPythonRun::
P3DPythonRun(int argc, char *argv[]) {
  _read_thread_continue = false;
  _program_continue = true;
  INIT_LOCK(_commands_lock);
  INIT_THREAD(_read_thread);

  _program_name = argv[0];
  _py_argc = 1;
  _py_argv = (char **)malloc(2 * sizeof(char *));
  _py_argv[0] = argv[0];
  _py_argv[1] = NULL;

  // Initialize Python.  It appears to be important to do this before
  // we open the pipe streams and spawn the thread, below.
  Py_SetProgramName((char *)_program_name.c_str());
  Py_Initialize();
  PySys_SetArgv(_py_argc, _py_argv);

  // Open the pipe streams with the input and output handles from the
  // parent.
#ifdef _WIN32
  HANDLE read = GetStdHandle(STD_INPUT_HANDLE);
  HANDLE write = GetStdHandle(STD_OUTPUT_HANDLE);
  if (!SetStdHandle(STD_INPUT_HANDLE, INVALID_HANDLE_VALUE)) {
    nout << "unable to reset input handle\n";
  }
  if (!SetStdHandle(STD_OUTPUT_HANDLE, INVALID_HANDLE_VALUE)) {
    nout << "unable to reset input handle\n";
  }

  _pipe_read.open_read(read);
  _pipe_write.open_write(write);
#else
  _pipe_read.open_read(STDIN_FILENO);
  _pipe_write.open_write(STDOUT_FILENO);
#endif  // _WIN32

  if (!_pipe_read) {
    nout << "unable to open read pipe\n";
  }
  if (!_pipe_write) {
    nout << "unable to open write pipe\n";
  }

  spawn_read_thread();
}

////////////////////////////////////////////////////////////////////
//     Function: P3DPythonRun::Destructor
//       Access: Public
//  Description: 
////////////////////////////////////////////////////////////////////
P3DPythonRun::
~P3DPythonRun() {
  Py_Finalize();

  join_read_thread();
  DESTROY_LOCK(_commands_lock);
}

////////////////////////////////////////////////////////////////////
//     Function: P3DPythonRun::run_python
//       Access: Public
//  Description: Runs the embedded Python process.  This method does
//               not return until the plugin is ready to exit.
////////////////////////////////////////////////////////////////////
bool P3DPythonRun::
run_python() {
  // First, load runp3d_frozen.pyd.  Since this is a magic frozen pyd,
  // importing it automatically makes all of its frozen contents
  // available to import as well.
  PyObject *runp3d_frozen = PyImport_ImportModule("runp3d_frozen");
  if (runp3d_frozen == NULL) {
    PyErr_Print();
    return false;
  }
  Py_DECREF(runp3d_frozen);

  // So now we can import the module itself.
  PyObject *runp3d = PyImport_ImportModule("direct.showutil.runp3d");
  if (runp3d == NULL) {
    PyErr_Print();
    return false;
  }

  // Get the pointers to the objects needed within the module.
  PyObject *app_runner_class = PyObject_GetAttrString(runp3d, "AppRunner");
  if (app_runner_class == NULL) {
    PyErr_Print();
    return false;
  }

  // Construct an instance of AppRunner.
  _runner = PyObject_CallFunction(app_runner_class, (char *)"");
  if (_runner == NULL) {
    PyErr_Print();
    return false;
  }
  Py_DECREF(app_runner_class);

  // Get the UndefinedObject class.
  _undefined_object_class = PyObject_GetAttrString(runp3d, "UndefinedObject");
  if (_undefined_object_class == NULL) {
    PyErr_Print();
    return false;
  }

  // And the "Undefined" instance.
  _undefined = PyObject_GetAttrString(runp3d, "Undefined");
  if (_undefined == NULL) {
    PyErr_Print();
    return false;
  }

  // Get the BrowserObject class.
  _browser_object_class = PyObject_GetAttrString(runp3d, "BrowserObject");
  if (_browser_object_class == NULL) {
    PyErr_Print();
    return false;
  }

  // Get the global TaskManager.
  _taskMgr = PyObject_GetAttrString(runp3d, "taskMgr");
  if (_taskMgr == NULL) {
    PyErr_Print();
    return false;
  }

  Py_DECREF(runp3d);


  // Construct a Python wrapper around our request_func() method.
  static PyMethodDef p3dpython_methods[] = {
    {"request_func", P3DPythonRun::st_request_func, METH_VARARGS,
     "Send an asynchronous request to the plugin host"},
    {NULL, NULL, 0, NULL}        /* Sentinel */
  };
  PyObject *p3dpython = Py_InitModule("p3dpython", p3dpython_methods);
  if (p3dpython == NULL) {
    PyErr_Print();
    return false;
  }
  PyObject *request_func = PyObject_GetAttrString(p3dpython, "request_func");
  if (request_func == NULL) {
    PyErr_Print();
    return false;
  }

  // Now pass that func pointer back to our AppRunner instance, so it
  // can call up to us.
  PyObject *result = PyObject_CallMethod(_runner, (char *)"setRequestFunc", (char *)"O", request_func);
  if (result == NULL) {
    PyErr_Print();
    return false;
  }
  Py_DECREF(result);
  Py_DECREF(request_func);
 

  // Now add check_comm() as a task.
  _check_comm_task = new GenericAsyncTask("check_comm", task_check_comm, this);
  AsyncTaskManager *task_mgr = AsyncTaskManager::get_global_ptr();
  task_mgr->add(_check_comm_task);

  // Finally, get lost in taskMgr.run().
  PyObject *done = PyObject_CallMethod(_taskMgr, (char *)"run", (char *)"");
  if (done == NULL) {
    PyErr_Print();
    return false;
  }
  Py_DECREF(done);

  return true;
}

////////////////////////////////////////////////////////////////////
//     Function: P3DPythonRun::handle_command
//       Access: Private
//  Description: Handles a command received from the plugin host, via
//               an XML syntax on the wire.
////////////////////////////////////////////////////////////////////
void P3DPythonRun::
handle_command(TiXmlDocument *doc) {
  nout << "received: " << *doc << "\n" << flush;
  TiXmlElement *xcommand = doc->FirstChildElement("command");
  if (xcommand != NULL) {
    bool needs_response = false;
    int want_response_id;
    if (xcommand->QueryIntAttribute("want_response_id", &want_response_id) == TIXML_SUCCESS) {
      // This command will be waiting for a response.
      needs_response = true;
    }

    const char *cmd = xcommand->Attribute("cmd");
    if (cmd != NULL) {
      if (strcmp(cmd, "start_instance") == 0) {
        assert(!needs_response);
        TiXmlElement *xinstance = xcommand->FirstChildElement("instance");
        if (xinstance != (TiXmlElement *)NULL) {
          P3DCInstance *inst = new P3DCInstance(xinstance);
          start_instance(inst, xinstance);
        }

      } else if (strcmp(cmd, "terminate_instance") == 0) {
        assert(!needs_response);
        int instance_id;
        if (xcommand->QueryIntAttribute("instance_id", &instance_id) == TIXML_SUCCESS) {
          terminate_instance(instance_id);
        }

      } else if (strcmp(cmd, "setup_window") == 0) {
        assert(!needs_response);
        int instance_id;
        TiXmlElement *xwparams = xcommand->FirstChildElement("wparams");
        if (xwparams != (TiXmlElement *)NULL && 
            xcommand->QueryIntAttribute("instance_id", &instance_id) == TIXML_SUCCESS) {
          setup_window(instance_id, xwparams);
        }

      } else if (strcmp(cmd, "exit") == 0) {
        assert(!needs_response);
        terminate_session();

      } else if (strcmp(cmd, "pyobj") == 0) {
        // Manipulate or query a python object.
        handle_pyobj_command(xcommand, needs_response, want_response_id);

      } else if (strcmp(cmd, "script_response") == 0) {
        // Response from a script request.
        assert(!needs_response);
        nout << "Ignoring unexpected script_response\n";
        
      } else {
        nout << "Unhandled command " << cmd << "\n";
        if (needs_response) {
          // Better send a response.
          TiXmlDocument doc;
          TiXmlDeclaration *decl = new TiXmlDeclaration("1.0", "utf-8", "");
          TiXmlElement *xresponse = new TiXmlElement("response");
          xresponse->SetAttribute("response_id", want_response_id);
          doc.LinkEndChild(decl);
          doc.LinkEndChild(xresponse);
          nout << "sent: " << doc << "\n" << flush;
          _pipe_write << doc << flush;
        }
      }
    }
  }
}

////////////////////////////////////////////////////////////////////
//     Function: P3DPythonRun::handle_pyobj_command
//       Access: Private
//  Description: Handles the pyobj command, which queries or modifies
//               a Python object from the browser scripts.
////////////////////////////////////////////////////////////////////
void P3DPythonRun::
handle_pyobj_command(TiXmlElement *xcommand, bool needs_response,
                     int want_response_id) {
  TiXmlDocument doc;
  TiXmlDeclaration *decl = new TiXmlDeclaration("1.0", "utf-8", "");
  TiXmlElement *xresponse = new TiXmlElement("response");
  xresponse->SetAttribute("response_id", want_response_id);
  doc.LinkEndChild(decl);
  doc.LinkEndChild(xresponse);

  const char *op = xcommand->Attribute("op");
  if (op != NULL) {
    if (strcmp(op, "get_panda_script_object") == 0) {
      // Get Panda's toplevel Python object.
      PyObject *obj = PyObject_CallMethod(_runner, (char*)"getPandaScriptObject", (char *)"");
      if (obj != NULL) {
        xresponse->LinkEndChild(pyobj_to_xml(obj));
        Py_DECREF(obj);
      }

    } else if (strcmp(op, "set_browser_script_object") == 0) {
      // Set the Browser's toplevel window object.
      PyObject *obj;
      TiXmlElement *xvalue = xcommand->FirstChildElement("value");
      if (xvalue != NULL) {
        obj = xml_to_pyobj(xvalue);
      } else {
        obj = Py_None;
        Py_INCREF(obj);
      }

      PyObject *result = PyObject_CallMethod
        (_runner, (char *)"setBrowserScriptObject", (char *)"O", obj);
      Py_DECREF(obj);
      Py_XDECREF(result);

    } else if (strcmp(op, "call") == 0) {
      // Call the named method on the indicated object, or the object
      // itself if method_name isn't given.
      int object_id;
      if (xcommand->QueryIntAttribute("object_id", &object_id) == TIXML_SUCCESS) {
        PyObject *obj = (PyObject *)(void *)object_id;
        const char *method_name = xcommand->Attribute("method_name");

        // Build up a list of params.
        PyObject *list = PyList_New(0);

        TiXmlElement *xchild = xcommand->FirstChildElement("value");
        while (xchild != NULL) {
          PyObject *child = xml_to_pyobj(xchild);
          PyList_Append(list, child);
          Py_DECREF(child);
          xchild = xchild->NextSiblingElement("value");
        }

        // Convert the list to a tuple for the call.
        PyObject *params = PyList_AsTuple(list);
        Py_DECREF(list);

        // Now call the method.
        PyObject *result = NULL;
        if (method_name == NULL) {
          // No method name; call the object directly.
          result = PyObject_CallObject(obj, params);
          
          // Several special-case "method" names.
        } else if (strcmp(method_name, "__bool__") == 0) {
          result = PyBool_FromLong(PyObject_IsTrue(obj));

        } else if (strcmp(method_name, "__int__") == 0) {
          result = PyNumber_Int(obj);

        } else if (strcmp(method_name, "__float__") == 0) {
          result = PyNumber_Float(obj);

        } else if (strcmp(method_name, "__repr__") == 0) {
          result = PyObject_Repr(obj);

        } else if (strcmp(method_name, "__str__") == 0 ||
                   strcmp(method_name, "toString") == 0) {
          result = PyObject_Str(obj);

        } else if (strcmp(method_name, "__setattr__") == 0) {
          char *property_name;
          PyObject *value;
          if (PyArg_ParseTuple(params, "sO", &property_name, &value)) {
            PyObject_SetAttrString(obj, property_name, value);
            result = Py_True;
            Py_INCREF(result);
          }

        } else if (strcmp(method_name, "__delattr__") == 0) {
          char *property_name;
          if (PyArg_ParseTuple(params, "s", &property_name)) {
            if (PyObject_HasAttrString(obj, property_name)) {
              PyObject_DelAttrString(obj, property_name);
              result = Py_True;
            } else {
              result = Py_False;
            }
            Py_INCREF(result);
          }

        } else if (strcmp(method_name, "__getattr__") == 0) {
          char *property_name;
          if (PyArg_ParseTuple(params, "s", &property_name)) {
            if (PyObject_HasAttrString(obj, property_name)) {
              result = PyObject_GetAttrString(obj, property_name);
            } else {
              result = NULL;
            }
          }

        } else if (strcmp(method_name, "__has_method__") == 0) {
          char *property_name;
          result = Py_False;
          if (PyArg_ParseTuple(params, "s", &property_name)) {
            if (*property_name) {
              // Check for a callable method
              if (PyObject_HasAttrString(obj, property_name)) {
                PyObject *prop = PyObject_GetAttrString(obj, property_name);
                if (PyCallable_Check(prop)) {
                  result = Py_True;
                }
                Py_DECREF(prop);
              }
            } else {
              // Check for the default method
              if (PyCallable_Check(obj)) {
                result = Py_True;
              }
            }
          }
          Py_INCREF(result);

        } else {
          // Not a special-case name.  Call the named method.
          PyObject *method = PyObject_GetAttrString(obj, (char *)method_name);
          if (method != NULL) {
            result = PyObject_CallObject(method, params);
            Py_DECREF(method);
          }
        }
        Py_DECREF(params);

        // Feed the return value back through the XML pipe to the
        // caller.
        if (result != NULL) {
          xresponse->LinkEndChild(pyobj_to_xml(result));
          Py_DECREF(result);
        }
      }
    }
  }

  if (needs_response) {
    nout << "sent: " << doc << "\n" << flush;
    _pipe_write << doc << flush;
  }
}

////////////////////////////////////////////////////////////////////
//     Function: P3DPythonRun::check_comm
//       Access: Private
//  Description: This method is added to the task manager (via
//               task_check_comm, below) so that it gets a call every
//               frame.  Its job is to check for commands received
//               from the plugin host in the parent process.
////////////////////////////////////////////////////////////////////
void P3DPythonRun::
check_comm() {
  ACQUIRE_LOCK(_commands_lock);
  while (!_commands.empty()) {
    TiXmlDocument *doc = _commands.front();
    _commands.pop_front();
    assert(_commands.size() < 10);
    RELEASE_LOCK(_commands_lock);
    handle_command(doc);
    delete doc;
    ACQUIRE_LOCK(_commands_lock);
  }

  if (!_program_continue) {
    // The low-level thread detected an error, for instance pipe
    // closed.  We should exit gracefully.
    terminate_session();
  }

  RELEASE_LOCK(_commands_lock);
}

////////////////////////////////////////////////////////////////////
//     Function: P3DPythonRun::task_check_comm
//       Access: Private, Static
//  Description: This static function wrapper around check_comm is
//               necessary to add the method function to the
//               GenericAsyncTask object.
////////////////////////////////////////////////////////////////////
AsyncTask::DoneStatus P3DPythonRun::
task_check_comm(GenericAsyncTask *task, void *user_data) {
  P3DPythonRun *self = (P3DPythonRun *)user_data;
  self->check_comm();
  return AsyncTask::DS_cont;
}

////////////////////////////////////////////////////////////////////
//     Function: P3DPythonRun::wait_script_response
//       Access: Private
//  Description: This method is similar to check_comm(), above, but
//               instead of handling all events, it waits for a
//               specific script_response ID to come back from the
//               browser, and leaves all other events in the queue.
////////////////////////////////////////////////////////////////////
TiXmlDocument *P3DPythonRun::
wait_script_response(int response_id) {
  nout << "Waiting script_response " << response_id << "\n";
  while (true) {
    ACQUIRE_LOCK(_commands_lock);
    
    Commands::iterator ci;
    for (ci = _commands.begin(); ci != _commands.end(); ++ci) {
      TiXmlDocument *doc = (*ci);

      TiXmlElement *xcommand = doc->FirstChildElement("command");
      if (xcommand != NULL) {
        const char *cmd = xcommand->Attribute("cmd");
        if (cmd != NULL && strcmp(cmd, "script_response") == 0) {
          int unique_id;
          if (xcommand->QueryIntAttribute("unique_id", &unique_id) == TIXML_SUCCESS) {
            if (unique_id == response_id) {
              // This is the response we were waiting for.
              _commands.erase(ci);
              RELEASE_LOCK(_commands_lock);
              nout << "received script_response: " << *doc << "\n" << flush;
              return doc;
            }
          }
        }

        // It's not the response we're waiting for, but maybe we need
        // to handle it anyway.
        bool needs_response = false;
        int want_response_id;
        if (xcommand->QueryIntAttribute("want_response_id", &want_response_id) == TIXML_SUCCESS) {
          // This command will be wanting a response.  We'd better
          // honor it right away, or we risk deadlock with the browser
          // process and the Python process waiting for each other.
          _commands.erase(ci);
          RELEASE_LOCK(_commands_lock);
          handle_command(doc);
          delete doc;
          ACQUIRE_LOCK(_commands_lock);
          break;
        }
      }
    }
    
    if (!_program_continue) {
      terminate_session();
    }
    
    RELEASE_LOCK(_commands_lock);

    // It hasn't shown up yet.  Give the sub-thread a chance to
    // process the input and append it to the queue.
    Thread::force_yield();
  }
  assert(false);
}

////////////////////////////////////////////////////////////////////
//     Function: P3DPythonRun::py_request_func
//       Access: Private
//  Description: This method is a special Python function that is
//               added as a callback to the AppRunner class, to allow
//               Python to upcall into this object.
////////////////////////////////////////////////////////////////////
PyObject *P3DPythonRun::
py_request_func(PyObject *args) {
  int instance_id;
  const char *request_type;
  PyObject *extra_args;
  if (!PyArg_ParseTuple(args, "isO", &instance_id, &request_type, &extra_args)) {
    return NULL;
  }

  if (strcmp(request_type, "wait_script_response") == 0) {
    // This is a special case.  Instead of generating a new request,
    // this means to wait for a particular script_response to come in
    // on the wire.
    int response_id;
    if (!PyArg_ParseTuple(extra_args, "i", &response_id)) {
      return NULL;
    }

    TiXmlDocument *doc = wait_script_response(response_id);
    assert(doc != NULL);
    TiXmlElement *xcommand = doc->FirstChildElement("command");
    assert(xcommand != NULL);
    TiXmlElement *xvalue = xcommand->FirstChildElement("value");

    PyObject *value = NULL;
    if (xvalue != NULL) {
      value = xml_to_pyobj(xvalue);
    } else {
      // An absence of a <value> element is an exception.  We will
      // return NULL from this function, but first set the error
      // condition.
      PyErr_SetString(PyExc_EnvironmentError, "Error on script call");
    }

    delete doc;
    return value;
  }

  TiXmlDocument doc;
  TiXmlDeclaration *decl = new TiXmlDeclaration("1.0", "utf-8", "");
  TiXmlElement *xrequest = new TiXmlElement("request");
  xrequest->SetAttribute("instance_id", instance_id);
  xrequest->SetAttribute("rtype", request_type);
  doc.LinkEndChild(decl);
  doc.LinkEndChild(xrequest);

  if (strcmp(request_type, "notify") == 0) {
    // A general notification to be sent directly to the instance.
    const char *message;
    if (!PyArg_ParseTuple(extra_args, "s", &message)) {
      return NULL;
    }

    xrequest->SetAttribute("message", message);
    nout << "sent: " << doc << "\n" << flush;
    _pipe_write << doc << flush;

  } else if (strcmp(request_type, "script") == 0) {
    // Meddling with a scripting variable on the browser side.
    const char *operation;
    PyObject *object;
    const char *property_name;
    PyObject *value;
    int needs_response;
    int unique_id;
    if (!PyArg_ParseTuple(extra_args, "sOsOii", 
                          &operation, &object, &property_name, &value, 
                          &needs_response, &unique_id)) {
      return NULL;
    }

    xrequest->SetAttribute("operation", operation);
    xrequest->SetAttribute("property_name", property_name);
    xrequest->SetAttribute("needs_response", (int)(needs_response != 0));
    xrequest->SetAttribute("unique_id", unique_id);
    TiXmlElement *xobject = pyobj_to_xml(object);
    xobject->SetValue("object");
    xrequest->LinkEndChild(xobject);

    if (strcmp(operation, "call") == 0 && PySequence_Check(value)) {
      // A special case: operation "call" receives a tuple of
      // parameters; unpack the tuple for the XML.
      Py_ssize_t length = PySequence_Length(value);
      for (Py_ssize_t i = 0; i < length; ++i) {
        PyObject *p = PySequence_GetItem(value, i);
        if (p != NULL) {
          TiXmlElement *xvalue = pyobj_to_xml(p);
          xrequest->LinkEndChild(xvalue);
          Py_DECREF(p);
        }
      }
      
    } else {
      // Other kinds of operations receive only a single parameter, if
      // any.
      TiXmlElement *xvalue = pyobj_to_xml(value);
      xrequest->LinkEndChild(xvalue);
    }

    nout << "sent: " << doc << "\n" << flush;
    _pipe_write << doc << flush;

  } else {
    string message = string("Unsupported request type: ") + string(request_type);
    PyErr_SetString(PyExc_ValueError, message.c_str());
    return NULL;
  }

  return Py_BuildValue("");
}

////////////////////////////////////////////////////////////////////
//     Function: P3DPythonRun::st_request_func
//       Access: Private, Static
//  Description: This is the static wrapper around py_request_func.
////////////////////////////////////////////////////////////////////
PyObject *P3DPythonRun::
st_request_func(PyObject *, PyObject *args) {
  return P3DPythonRun::_global_ptr->py_request_func(args);
}

////////////////////////////////////////////////////////////////////
//     Function: P3DPythonRun::spawn_read_thread
//       Access: Private
//  Description: Starts the read thread.  This thread is responsible
//               for reading the standard input socket for XML
//               commands and storing them in the _commands queue.
////////////////////////////////////////////////////////////////////
void P3DPythonRun::
spawn_read_thread() {
  assert(!_read_thread_continue);

  // We have to use direct OS calls to create the thread instead of
  // Panda constructs, because it has to be an actual thread, not
  // necessarily a Panda thread (we can't use Panda's simple threads
  // implementation, because we can't get overlapped I/O on an
  // anonymous pipe in Windows).

  _read_thread_continue = true;
  SPAWN_THREAD(_read_thread, rt_thread_run, this);
}

////////////////////////////////////////////////////////////////////
//     Function: P3DPythonRun::join_read_thread
//       Access: Private
//  Description: Waits for the read thread to stop.
////////////////////////////////////////////////////////////////////
void P3DPythonRun::
join_read_thread() {
  _read_thread_continue = false;
  _pipe_read.close();

  JOIN_THREAD(_read_thread);
}

////////////////////////////////////////////////////////////////////
//     Function: P3DPythonRun::start_instance
//       Access: Private
//  Description: Starts the indicated instance running within the
//               Python process.
////////////////////////////////////////////////////////////////////
void P3DPythonRun::
start_instance(P3DCInstance *inst, TiXmlElement *xinstance) {
  _instances[inst->get_instance_id()] = inst;

  TiXmlElement *xfparams = xinstance->FirstChildElement("fparams");
  if (xfparams != (TiXmlElement *)NULL) {
    set_p3d_filename(inst, xfparams);
  }

  TiXmlElement *xwparams = xinstance->FirstChildElement("wparams");
  if (xwparams != (TiXmlElement *)NULL) {
    setup_window(inst, xwparams);
  }
}

////////////////////////////////////////////////////////////////////
//     Function: P3DPythonRun::terminate_instance
//       Access: Private
//  Description: Stops the instance with the indicated id.
////////////////////////////////////////////////////////////////////
void P3DPythonRun::
terminate_instance(int id) {
  Instances::iterator ii = _instances.find(id);
  if (ii == _instances.end()) {
    nout << "Can't stop instance " << id << ": not started.\n";
    return;
  }

  P3DCInstance *inst = (*ii).second;
  _instances.erase(ii);
  delete inst;

  // TODO: we don't currently have any way to stop just one instance
  // of a multi-instance session.  This will require a different
  // Python interface than ShowBase.
  terminate_session();
}

////////////////////////////////////////////////////////////////////
//     Function: P3DPythonRun::set_p3d_filename
//       Access: Private
//  Description: Sets the startup filename and tokens for the
//               indicated instance.
////////////////////////////////////////////////////////////////////
void P3DPythonRun::
set_p3d_filename(P3DCInstance *inst, TiXmlElement *xfparams) {
  string p3d_filename;
  const char *p3d_filename_c = xfparams->Attribute("p3d_filename");
  if (p3d_filename_c != NULL) {
    p3d_filename = p3d_filename_c;
  }

  PyObject *token_list = PyList_New(0);

  TiXmlElement *xtoken = xfparams->FirstChildElement("token");
  while (xtoken != NULL) {
    string keyword, value;
    const char *keyword_c = xtoken->Attribute("keyword");
    if (keyword_c != NULL) {
      keyword = keyword_c;
    }

    const char *value_c = xtoken->Attribute("value");
    if (value_c != NULL) {
      value = value_c;
    }

    PyObject *tuple = Py_BuildValue("(ss)", keyword.c_str(), 
                                    value.c_str());
    PyList_Append(token_list, tuple);
    Py_DECREF(tuple);

    xtoken = xtoken->NextSiblingElement("token");
  }
  
  PyObject *result = PyObject_CallMethod
    (_runner, (char *)"setP3DFilename", (char *)"sOi", p3d_filename.c_str(),
     token_list, inst->get_instance_id());
  Py_DECREF(token_list);

  if (result == NULL) {
    PyErr_Print();
  }
  Py_XDECREF(result);
}

////////////////////////////////////////////////////////////////////
//     Function: P3DPythonRun::setup_window
//       Access: Private
//  Description: Sets the window parameters for the indicated instance.
////////////////////////////////////////////////////////////////////
void P3DPythonRun::
setup_window(int id, TiXmlElement *xwparams) {
  Instances::iterator ii = _instances.find(id);
  if (ii == _instances.end()) {
    nout << "Can't setup window for " << id << ": not started.\n";
    return;
  }

  P3DCInstance *inst = (*ii).second;
  setup_window(inst, xwparams);
}

////////////////////////////////////////////////////////////////////
//     Function: P3DPythonRun::setup_window
//       Access: Private
//  Description: Sets the window parameters for the indicated instance.
////////////////////////////////////////////////////////////////////
void P3DPythonRun::
setup_window(P3DCInstance *inst, TiXmlElement *xwparams) {
  string window_type;
  const char *window_type_c = xwparams->Attribute("window_type");
  if (window_type_c != NULL) {
    window_type = window_type_c;
  }

  int win_x, win_y, win_width, win_height;
  
  xwparams->Attribute("win_x", &win_x);
  xwparams->Attribute("win_y", &win_y);
  xwparams->Attribute("win_width", &win_width);
  xwparams->Attribute("win_height", &win_height);

  long parent_window_handle = 0;

#ifdef _WIN32
  int hwnd;
  if (xwparams->Attribute("parent_hwnd", &hwnd)) {
    parent_window_handle = (long)hwnd;
  }
#endif
#ifdef HAVE_X11
  // Bad! Casting to int loses precision.
  int xwindow;
  if (xwparams->Attribute("parent_xwindow", &xwindow)) {
    parent_window_handle = (unsigned long)xwindow;
  }
#endif

  // TODO: direct this into the particular instance.  This will
  // require a specialized ShowBase replacement.
  PyObject *result = PyObject_CallMethod
    (_runner, (char *)"setupWindow", (char *)"siiiii", window_type.c_str(),
     win_x, win_y, win_width, win_height,
     parent_window_handle);
  if (result == NULL) {
    PyErr_Print();
  }
  Py_XDECREF(result);
}


////////////////////////////////////////////////////////////////////
//     Function: P3DPythonRun::terminate_session
//       Access: Private
//  Description: Stops all currently-running instances.
////////////////////////////////////////////////////////////////////
void P3DPythonRun::
terminate_session() {
  Instances::iterator ii;
  for (ii = _instances.begin(); ii != _instances.end(); ++ii) {
    P3DCInstance *inst = (*ii).second;
    delete inst;
  }
  _instances.clear();

  PyObject *result = PyObject_CallMethod(_taskMgr, (char *)"stop", (char *)"");
  if (result == NULL) {
    PyErr_Print();
    return;
  }
  Py_DECREF(result);

  // The task manager is cleaned up.  Let's exit immediately here,
  // rather than returning all the way up.  This just makes it easier
  // when we call terminate_session() from a deeply-nested loop.
  exit(0);
}

////////////////////////////////////////////////////////////////////
//     Function: P3DPythonRun::pyobj_to_xml
//       Access: Private
//  Description: Converts the indicated PyObject to the appropriate
//               XML representation of a P3D_value type, and returns a
//               freshly-allocated TiXmlElement.
////////////////////////////////////////////////////////////////////
TiXmlElement *P3DPythonRun::
pyobj_to_xml(PyObject *value) {
  TiXmlElement *xvalue = new TiXmlElement("value");
  if (value == Py_None) {
    // None.
    xvalue->SetAttribute("type", "none");

  } else if (PyBool_Check(value)) {
    // A bool value.
    xvalue->SetAttribute("type", "bool");
    xvalue->SetAttribute("value", PyObject_IsTrue(value));

  } else if (PyInt_Check(value)) {
    // A plain integer value.
    xvalue->SetAttribute("type", "int");
    xvalue->SetAttribute("value", PyInt_AsLong(value));

  } else if (PyLong_Check(value)) {
    // A long integer value.  This gets converted either as an integer
    // or as a floating-point type, whichever fits.
    long lvalue = PyLong_AsLong(value);
    if (PyErr_Occurred()) {
      // It won't fit as an integer; make it a double.
      PyErr_Clear();
      xvalue->SetAttribute("type", "float");
      xvalue->SetDoubleAttribute("value", PyLong_AsDouble(value));
    } else {
      // It fits as an integer.
      xvalue->SetAttribute("type", "int");
      xvalue->SetAttribute("value", lvalue);
    }

  } else if (PyFloat_Check(value)) {
    // A floating-point value.
    xvalue->SetAttribute("type", "float");
    xvalue->SetDoubleAttribute("value", PyFloat_AsDouble(value));

  } else if (PyUnicode_Check(value)) {
    // A unicode value.  Convert to utf-8 for the XML encoding.
    xvalue->SetAttribute("type", "string");
    PyObject *as_str = PyUnicode_AsUTF8String(value);
    if (as_str != NULL) {
      char *buffer;
      Py_ssize_t length;
      if (PyString_AsStringAndSize(as_str, &buffer, &length) != -1) {
        string str(buffer, length);
        xvalue->SetAttribute("value", str);
      }
      Py_DECREF(as_str);
    }

  } else if (PyString_Check(value)) {
    // A string value.
    xvalue->SetAttribute("type", "string");

    char *buffer;
    Py_ssize_t length;
    if (PyString_AsStringAndSize(value, &buffer, &length) != -1) {
      string str(buffer, length);
      xvalue->SetAttribute("value", str);
    }

  } else if (PyObject_IsInstance(value, _undefined_object_class)) {
    // This is an UndefinedObject.
    xvalue->SetAttribute("type", "undefined");

  } else if (PyObject_IsInstance(value, _browser_object_class)) {
    // This is a BrowserObject, a reference to an object that actually
    // exists in the host namespace.  So, pass up the appropriate
    // object ID.
    PyObject *objectId = PyObject_GetAttrString(value, (char *)"_BrowserObject__objectId");
    if (objectId != NULL) {
      int object_id = PyInt_AsLong(objectId);
      xvalue->SetAttribute("type", "browser");
      xvalue->SetAttribute("object_id", object_id);
      Py_DECREF(objectId);
    }

  } else {
    // Some other kind of object.  Make it a generic Python object.
    // This is more expensive for the caller to deal with--it requires
    // a back-and-forth across the XML pipe--but it's much more
    // general.
    // TODO: pass pointers better.
    xvalue->SetAttribute("type", "python");
    xvalue->SetAttribute("object_id", (int)(intptr_t)value);

    // TODO: fix this hack, properly manage these reference counts.
    Py_INCREF(value);
  }

  return xvalue;
}

////////////////////////////////////////////////////////////////////
//     Function: P3DPythonRun::xml_to_pyobj
//       Access: Private
//  Description: Converts the XML representation of a P3D_value type
//               into the equivalent Python object and returns it.
////////////////////////////////////////////////////////////////////
PyObject *P3DPythonRun::
xml_to_pyobj(TiXmlElement *xvalue) {
  const char *type = xvalue->Attribute("type");
  if (strcmp(type, "none") == 0) {
    return Py_BuildValue("");

  } else if (strcmp(type, "bool") == 0) {
    int value;
    if (xvalue->QueryIntAttribute("value", &value) == TIXML_SUCCESS) {
      return PyBool_FromLong(value);
    }

  } else if (strcmp(type, "int") == 0) {
    int value;
    if (xvalue->QueryIntAttribute("value", &value) == TIXML_SUCCESS) {
      return PyInt_FromLong(value);
    }

  } else if (strcmp(type, "float") == 0) {
    double value;
    if (xvalue->QueryDoubleAttribute("value", &value) == TIXML_SUCCESS) {
      return PyFloat_FromDouble(value);
    }

  } else if (strcmp(type, "string") == 0) {
    // Using the string form here instead of the char * form, so we
    // don't get tripped up on embedded null characters.
    const string *value = xvalue->Attribute(string("value"));
    if (value != NULL) {
      return PyString_FromStringAndSize(value->data(), value->length());
    }

  } else if (strcmp(type, "undefined") == 0) {
    Py_INCREF(_undefined);
    return _undefined;

  } else if (strcmp(type, "browser") == 0) {
    int object_id;
    if (xvalue->QueryIntAttribute("object_id", &object_id) == TIXML_SUCCESS) {
      // Construct a new BrowserObject wrapper around this object.
      return PyObject_CallFunction(_browser_object_class, (char *)"Oi", 
                                   _runner, object_id);
    }

  } else if (strcmp(type, "python") == 0) {
    int object_id;
    if (xvalue->QueryIntAttribute("object_id", &object_id) == TIXML_SUCCESS) {
      return (PyObject *)(void *)object_id;
    }
  }

  // Something went wrong in decoding.
  return Py_BuildValue("");
}

////////////////////////////////////////////////////////////////////
//     Function: P3DPythonRun::rt_thread_run
//       Access: Private
//  Description: The main function for the read thread.
////////////////////////////////////////////////////////////////////
void P3DPythonRun::
rt_thread_run() {
  while (_read_thread_continue) {
    TiXmlDocument *doc = new TiXmlDocument;

    _pipe_read >> *doc;
    if (!_pipe_read || _pipe_read.eof()) {
      // Some error on reading.  Abort.
      _program_continue = false;
      return;
    }

    // Successfully read an XML document.
    
    // Check for one special case: the "exit" command means we shut
    // down the read thread along with everything else.
    TiXmlElement *xcommand = doc->FirstChildElement("command");
    if (xcommand != NULL) {
      const char *cmd = xcommand->Attribute("cmd");
      if (cmd != NULL) {
        if (strcmp(cmd, "exit") == 0) {
          _read_thread_continue = false;
        }
      }
    }

    // Feed the command up to the parent.
    ACQUIRE_LOCK(_commands_lock);
    _commands.push_back(doc);
    RELEASE_LOCK(_commands_lock);
  }
}

////////////////////////////////////////////////////////////////////
//     Function: main
//  Description: Starts the program running.
////////////////////////////////////////////////////////////////////
int
main(int argc, char *argv[]) {
  P3DPythonRun::_global_ptr = new P3DPythonRun(argc, argv);
  
  if (!P3DPythonRun::_global_ptr->run_python()) {
    nout << "Couldn't initialize Python.\n";
    return 1;
  }
  return 0;
}