/*
* Copyright (c) 2014, 2017, Oracle and/or its affiliates. All rights reserved.
*
* This program is free software; you can redistribute it and/or
* modify it under the terms of the GNU General Public License as
* published by the Free Software Foundation; version 2 of the
* License.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
* 02110-1301  USA
*/

#include "utils/utils_sqlstring.h"
#include "mod_mysqlx_session.h"
#include "mod_mysqlx_schema.h"
#include "mod_mysqlx_resultset.h"
#include "mod_mysqlx_expression.h"
#include "mod_mysqlx_constants.h"
#include "shellcore/object_factory.h"
#include "shellcore/shell_core.h"
#include "shellcore/lang_base.h"
#include "mod_mysqlx_session_sql.h"
#include "utils/utils_general.h"
#include "utils/utils_time.h"
#include "utils/utils_file.h"
#include "shellcore/proxy_object.h"
#include "shellcore/shell_notifications.h"

#include "mysqlxtest_utils.h"

#include <boost/lexical_cast.hpp>
#include <boost/format.hpp>
#include <boost/algorithm/string.hpp>
#include "mysqlx_connection.h"
#include "logger/logger.h"

#include <stdlib.h>
#include <string.h>

#include "utils/utils_help.h"

#define MAX_COLUMN_LENGTH 1024
#define MIN_COLUMN_LENGTH 4

#if _MSC_VER
#define _CRT_SECURE_NO_WARNINGS
#define mystrdup _strdup
#else
#define mystrdup strdup
#endif

using namespace mysqlsh;
using namespace shcore;
using namespace mysqlsh::mysqlx;

REGISTER_OBJECT(mysqlx, XSession);
REGISTER_OBJECT(mysqlx, NodeSession);
REGISTER_OBJECT(mysqlx, Expression);
REGISTER_OBJECT(mysqlx, Type);
REGISTER_OBJECT(mysqlx, IndexType);

#include <set>

#ifdef WIN32
#define strcasecmp _stricmp
#endif

// Documentation of BaseSession class
REGISTER_HELP(BASESESSION_BRIEF, "Base functionality for Session classes through the X Protocol.");
REGISTER_HELP(BASESESSION_DETAIL, "This class encloses the core functionality to be made available on both the XSession and "\
"NodeSession classes, such functionality includes");
REGISTER_HELP(BASESESSION_DETAIL1, "@li Accessing available schemas.");
REGISTER_HELP(BASESESSION_DETAIL2, "@li Schema management operations.");
REGISTER_HELP(BASESESSION_DETAIL3, "@li Enabling/disabling warning generation.");
REGISTER_HELP(BASESESSION_DETAIL4, "@li Retrieval of connection information.");

// Documentation of XSession class
REGISTER_HELP(XSESSION_INTERACTIVE_BRIEF, "Represents the currently open MySQL session.");
REGISTER_HELP(XSESSION_BRIEF, "Enables interaction with an X Protocol enabled MySQL Product.");
REGISTER_HELP(XSESSION_DETAIL, "Note that this class inherits the behavior described on the BaseSession class.");
REGISTER_HELP(XSESSION_DETAIL1, "In the future this class will be improved to support interacting not only with MySQL Server "\
"but with other products.");

// Documentation of NodeSession class
REGISTER_HELP(NODESESSION_INTERACTIVE_BRIEF, "Represents the currently open MySQL session.");
REGISTER_HELP(NODESESSION_BRIEF, "Enables interaction with an X Protocol enabled MySQL Server, this includes SQL Execution.");
REGISTER_HELP(NODESESSION_DETAIL, "Note that this class inherits the behavior described on the BaseSession class.");

BaseSession::BaseSession()
  : _case_sensitive_table_names(false) {
  init();
}

// Documentation of isOpen function
REGISTER_HELP(BASESESSION_ISOPEN_BRIEF, "Verifies if the session is still open.");
/**
* $(BASESESSION_ISOPEN_BRIEF)
*/
#if DOXYGEN_JS
Bool BaseSession::isOpen() {}
#elif DOXYGEN_PY
bool BaseSession::is_open() {}
#endif
bool BaseSession::is_connected() const {
  return _session.is_connected();
}

std::shared_ptr< ::mysqlx::Session> BaseSession::session_obj() const {
  return _session.get();
}

BaseSession::BaseSession(const BaseSession& s) : ShellDevelopmentSession(s), _case_sensitive_table_names(false) {
  init();
}

void BaseSession::init() {
  _schemas.reset(new shcore::Value::Map_type);

  add_method("close", std::bind(&BaseSession::close, this, _1), "data");
  add_method("setFetchWarnings", std::bind(&BaseSession::set_fetch_warnings, this, _1), "data");
  add_method("startTransaction", std::bind(&BaseSession::startTransaction, this, _1), "data");
  add_method("commit", std::bind(&BaseSession::commit, this, _1), "data");
  add_method("rollback", std::bind(&BaseSession::rollback, this, _1), "data");

  add_method("dropSchema", std::bind(&BaseSession::drop_schema, this, _1), "data");
  add_method("dropTable", std::bind(&BaseSession::drop_schema_object, this, _1, "Table"), "data");
  add_method("dropCollection", std::bind(&BaseSession::drop_schema_object, this, _1, "Collection"), "data");
  add_method("dropView", std::bind(&BaseSession::drop_schema_object, this, _1, "View"), "data");

  // Prepares the cache handling
  auto generator = [this](const std::string& name) {return shcore::Value::wrap<Schema>(new Schema(_get_shared_this(), name)); };
  update_schema_cache = [generator, this](const std::string &name, bool exists) {DatabaseObject::update_cache(name, generator, true, _schemas); };
}

Value BaseSession::connect(const Argument_list &args) {
  std::string function = class_name() + '.' + "connect";
  args.ensure_count(1, 2, function.c_str());

  try {
    // Retrieves the connection data, whatever the source is
    load_connection_data(args);

    unsigned int ssl_mode;
    if (_ssl_info.skip)
      ssl_mode = static_cast<int>(shcore::SslMode::Disabled);
    else {
      if (_ssl_info.mode)
        ssl_mode = _ssl_info.mode;
      else
        ssl_mode = static_cast<int>(shcore::SslMode::Preferred);
    }

    _session.open(_host, _port, _schema, _user, _password, _ssl_info.ca,
      _ssl_info.cert, _ssl_info.key, _ssl_info.capath, _ssl_info.crl, _ssl_info.crlpath,
      _ssl_info.tls_version, _ssl_info.ciphers, ssl_mode, 60000, _auth_method, true);

    _default_schema = _schema;
    if (!_default_schema.empty())
      update_schema_cache(_default_schema, true);
  }
  CATCH_AND_TRANSLATE();

  return Value::Null();
}

void BaseSession::set_option(const char *option, int value) {
  if (strcmp(option, "trace_protocol") == 0 && _session.is_connected())
    _session.enable_protocol_trace(value != 0);
  else
    throw shcore::Exception::argument_error(std::string("Unknown option ").append(option));
}

uint64_t BaseSession::get_connection_id() const {
  return _connection_id;
}

bool BaseSession::table_name_compare(const std::string &n1, const std::string &n2) {
  if (_case_sensitive_table_names)
    return n1 == n2;
  else
    return strcasecmp(n1.c_str(), n2.c_str()) == 0;
}

// Documentation of close function
REGISTER_HELP(BASESESSION_CLOSE_BRIEF, "Closes the session.");
REGISTER_HELP(BASESESSION_CLOSE_DETAIL, "After closing the session it is still possible to make read only operation "\
"to gather metadata info, like getTable(name) or getSchemas().");

/**
* $(BASESESSION_CLOSE_BRIEF)
*
* $(BASESESSION_CLOSE_DETAIL)
*/
#if DOXYGEN_JS
Undefined BaseSession::close() {}
#elif DOXYGEN_PY
None BaseSession::close() {}
#endif
Value BaseSession::close(const shcore::Argument_list &args) {
  args.ensure_count(0, get_function_name("close").c_str());

  // Connection must be explicitly closed, we can't rely on the
  // automatic destruction because if shared across different objects
  // it may remain open
  reset_session();

  ShellNotifications::get()->notify("SN_SESSION_CLOSED", _get_shared_this());

  return shcore::Value();
}

void BaseSession::reset_session() {
  try {
    log_warning("Closing session: %s", _uri.c_str());

    _session.reset();
  } catch (std::exception &e) {
    log_warning("Error occurred closing session: %s", e.what());
  }
}

Value BaseSession::sql(const Argument_list &args) {
  args.ensure_count(1, get_function_name("sql").c_str());

  // NOTE: This function is no longer exposed as part of the API but it kept
  //       since it is used internally.
  //       until further cleanup is done let it live here.
  Value ret_val;
  try {
    ret_val = execute_sql(args.string_at(0), shcore::Argument_list());
  }
  CATCH_AND_TRANSLATE();

  return ret_val;
}

// Documentation of createSchema function
REGISTER_HELP(BASESESSION_CREATESCHEMA_BRIEF, "Creates a schema on the database and returns the corresponding object.");
REGISTER_HELP(BASESESSION_CREATESCHEMA_PARAM, "@param name A string value indicating the schema name.");
REGISTER_HELP(BASESESSION_CREATESCHEMA_RETURN, "@return The created schema object.");
REGISTER_HELP(BASESESSION_CREATESCHEMA_EXCEPTION, "@exception An exception is thrown if an error occurs creating the XSession.");

/**
* $(BASESESSION_CREATESCHEMA_BRIEF)
*
* $(BASESESSION_CREATESCHEMA_PARAM)
* $(BASESESSION_CREATESCHEMA_RETURN)
*
* $(BASESESSION_CREATESCHEMA_EXCEPTION)
*/
#if DOXYGEN_JS
Schema BaseSession::createSchema(String name) {}
#elif DOXYGEN_PY
Schema BaseSession::create_schema(str name) {}
#endif
Value BaseSession::create_schema(const shcore::Argument_list &args) {
  args.ensure_count(1, get_function_name("createSchema").c_str());

  Value ret_val;
  try {
    std::string schema = args.string_at(0);
    std::string statement = sqlstring("create schema !", 0) << schema;
    ret_val = executeStmt("sql", statement, false, shcore::Argument_list());

    // if reached this point it indicates that there were no errors
    update_schema_cache(schema, true);

    ret_val = (*_schemas)[schema];
  }
  CATCH_AND_TRANSLATE_FUNCTION_EXCEPTION(get_function_name("createSchema"));

  return ret_val;
}

// Documentation of startTransaction function
REGISTER_HELP(BASESESSION_STARTTRANSACTION_BRIEF, "Starts a transaction context on the server.");
REGISTER_HELP(BASESESSION_STARTTRANSACTION_RETURN, "@return A SqlResult object.");
REGISTER_HELP(BASESESSION_STARTTRANSACTION_DETAIL, "Calling this function will turn off the autocommit mode on the server.");
REGISTER_HELP(BASESESSION_STARTTRANSACTION_DETAIL1, "All the operations executed after calling this function will take place only when commit() is called.");
REGISTER_HELP(BASESESSION_STARTTRANSACTION_DETAIL2, "All the operations executed after calling this function, will be discarded is rollback() is called.");
REGISTER_HELP(BASESESSION_STARTTRANSACTION_DETAIL3, "When commit() or rollback() are called, the server autocommit mode "\
"will return back to it's state before calling startTransaction().");

/**
* $(BASESESSION_STARTTRANSACTION_BRIEF)
*
* $(BASESESSION_STARTTRANSACTION_RETURN)
*
* $(BASESESSION_STARTTRANSACTION_DETAIL)
*
* $(BASESESSION_STARTTRANSACTION_DETAIL1)
* $(BASESESSION_STARTTRANSACTION_DETAIL2)
* $(BASESESSION_STARTTRANSACTION_DETAIL3)
*/
#if DOXYGEN_JS
Result BaseSession::startTransaction() {}
#elif DOXYGEN_PY
Result BaseSession::start_transaction() {}
#endif
shcore::Value BaseSession::startTransaction(const shcore::Argument_list &args) {
  args.ensure_count(0, get_function_name("startTransaction").c_str());

  shcore::Value ret_val;
  try {
    ret_val = executeStmt("sql", "start transaction", false, shcore::Argument_list());
  }
  CATCH_AND_TRANSLATE_FUNCTION_EXCEPTION(get_function_name("startTransaction"));

  return ret_val;
}

// Documentation of commit function
REGISTER_HELP(BASESESSION_COMMIT_BRIEF, "Commits all the operations executed after a call to startTransaction().");
REGISTER_HELP(BASESESSION_COMMIT_RETURN, "@return A SqlResult object.");
REGISTER_HELP(BASESESSION_COMMIT_DETAIL, "All the operations executed after calling startTransaction() will take place when this function is called.");
REGISTER_HELP(BASESESSION_COMMIT_DETAIL1, "The server autocommit mode will return back to it's state before calling startTransaction().");

/**
* $(BASESESSION_COMMIT_BRIEF)
*
* $(BASESESSION_COMMIT_RETURN)
*
* $(BASESESSION_COMMIT_DETAIL)
*
* $(BASESESSION_COMMIT_DETAIL1)
*/
#if DOXYGEN_JS
Result BaseSession::commit() {}
#elif DOXYGEN_PY
Result BaseSession::commit() {}
#endif
shcore::Value BaseSession::commit(const shcore::Argument_list &args) {
  args.ensure_count(0, get_function_name("commit").c_str());

  shcore::Value ret_val;

  try {
    ret_val = executeStmt("sql", "commit", false, shcore::Argument_list());
  }
  CATCH_AND_TRANSLATE_FUNCTION_EXCEPTION(get_function_name("commit"));

  return ret_val;
}

// Documentation of rollback function
REGISTER_HELP(BASESESSION_ROLLBACK_BRIEF, "Discards all the operations executed after a call to startTransaction().");
REGISTER_HELP(BASESESSION_ROLLBACK_RETURN, "@return A SqlResult object.");
REGISTER_HELP(BASESESSION_ROLLBACK_DETAIL, "All the operations executed after calling startTransaction() will be discarded when this function is called.");
REGISTER_HELP(BASESESSION_ROLLBACK_DETAIL1, "The server autocommit mode will return back to it's state before calling startTransaction().");

/**
* $(BASESESSION_ROLLBACK_BRIEF)
*
* $(BASESESSION_ROLLBACK_RETURN)
*
* $(BASESESSION_ROLLBACK_DETAIL)
*
* $(BASESESSION_ROLLBACK_DETAIL1)
*/
#if DOXYGEN_JS
Result BaseSession::rollback() {}
#elif DOXYGEN_PY
Result BaseSession::rollback() {}
#endif
shcore::Value BaseSession::rollback(const shcore::Argument_list &args) {
  args.ensure_count(0, get_function_name("rollback").c_str());

  shcore::Value ret_val;

  try {
    ret_val = executeStmt("sql", "rollback", false, shcore::Argument_list());
  }
  CATCH_AND_TRANSLATE_FUNCTION_EXCEPTION(get_function_name("rollback"));

  return ret_val;
}

/* Sql Execution Function */
shcore::Value BaseSession::execute_sql(const std::string& statement, const shcore::Argument_list &args) const {
  return executeStmt("sql", statement, true, args);
}

// This function is for SQL execution at the C++ layer
std::shared_ptr< ::mysqlx::Result> BaseSession::execute_sql(const std::string &sql) const {
  std::shared_ptr< ::mysqlx::Result> result;
  try {
    result = _session.execute_sql(sql);
  } catch (const ::mysqlx::Error &e) {
    if (e.error() == 2006 || e.error() == 5166 || e.error() == 2013) {
      std::shared_ptr<BaseSession> myself = std::dynamic_pointer_cast<BaseSession>(_get_shared_this());
      ShellNotifications::get()->notify("SN_SESSION_CONNECTION_LOST", std::dynamic_pointer_cast<Cpp_object_bridge>(myself));
    }

    // Rethrows the exception for normal flow
    throw;
  }

  return result;
}

Value BaseSession::executeAdminCommand(const std::string& command, bool expect_data, const Argument_list &args) const {
  std::string function = class_name() + '.' + "executeAdminCommand";
  args.ensure_at_least(1, function.c_str());

  return executeStmt("xplugin", command, expect_data, args);
}

Value BaseSession::executeStmt(const std::string &domain, const std::string& command, bool expect_data, const Argument_list &args) const {
  MySQL_timer timer;
  Value ret_val;

  try {
    timer.start();

    std::shared_ptr< ::mysqlx::Result> exec_result = _session.execute_statement(domain, command, args);

    timer.end();

    if (expect_data) {
      SqlResult *result = new SqlResult(exec_result);
      result->set_execution_time(timer.raw_duration());
      ret_val = shcore::Value::wrap(result);
    } else {
      Result *result = new Result(exec_result);
      result->set_execution_time(timer.raw_duration());
      ret_val = shcore::Value::wrap(result);
    }
  } catch (const ::mysqlx::Error &e) {
    if (e.error() == 2006 || e.error() == 5166 || e.error() == 2013) {
      std::shared_ptr<BaseSession> myself = std::dynamic_pointer_cast<BaseSession>(_get_shared_this());
      ShellNotifications::get()->notify("SN_SESSION_CONNECTION_LOST", std::dynamic_pointer_cast<Cpp_object_bridge>(myself));
    }

    // Rethrows the exception for normal flow
    throw;
  }

  return ret_val;
}

// Documentation of getDefaultSchema function
REGISTER_HELP(BASESESSION_GETDEFAULTSCHEMA_BRIEF, "Retrieves the Schema configured as default for the session.");
REGISTER_HELP(BASESESSION_GETDEFAULTSCHEMA_RETURN, "@return A Schema object or Null");

#if DOXYGEN_JS || DOXYGEN_PY
/**
* $(BASESESSION_GETDEFAULTSCHEMA_BRIEF)
*
* $(BASESESSION_GETDEFAULTSCHEMA_RETURN)
*/
#if DOXYGEN_JS
Schema BaseSession::getDefaultSchema() {}
#elif DOXYGEN_PY
Schema BaseSession::get_default_schema() {}
#endif

// Documentation of getUri function
REGISTER_HELP(BASESESSION_GETURI_BRIEF, "Retrieves the connection data for this session in string format.");
REGISTER_HELP(BASESESSION_GETURI_RETURN, "@return A string representing the connection data.");

/**
* $(BASESESSION_GETURI_BRIEF)
*
* $(BASESESSION_GETURI_RETURN)
*/
#if DOXYGEN_JS
String BaseSession::getUri() {}
#elif DOXYGEN_PY
str BaseSession::get_uri() {}
#endif
#endif

std::string BaseSession::_retrieve_current_schema() {
  std::string name;
  try {
    if (_session.is_connected()) {
      // TODO: update this logic properly
      std::shared_ptr< ::mysqlx::Result> result = execute_sql("select schema()");
      std::shared_ptr< ::mysqlx::Row>row = result->next();

      if (!row->isNullField(0))
        name = row->stringField(0);

      result->flush();
    }
  }
  CATCH_AND_TRANSLATE();

  return name;
}

// Documentation of getSchema function
REGISTER_HELP(BASESESSION_GETSCHEMA_BRIEF, "Retrieves a Schema object from the current session through it's name.");
REGISTER_HELP(BASESESSION_GETSCHEMA_PARAM, "@param name The name of the Schema object to be retrieved.");
REGISTER_HELP(BASESESSION_GETSCHEMA_RETURN, "@return The Schema object with the given name.");
REGISTER_HELP(BASESESSION_GETSCHEMA_EXCEPTION, "@exception An exception is thrown if the given name is not a valid schema on the XSession.");

/**
* $(BASESESSION_GETSCHEMA_BRIEF)
*
* $(BASESESSION_GETSCHEMA_PARAM)
* $(BASESESSION_GETSCHEMA_RETURN)
* $(BASESESSION_GETSCHEMA_EXCEPTION)
* \sa Schema
*/
#if DOXYGEN_JS
Schema BaseSession::getSchema(String name) {}
#elif DOXYGEN_PY
Schema BaseSession::get_schema(str name) {}
#endif
shcore::Value BaseSession::get_schema(const shcore::Argument_list &args) const {
  args.ensure_count(1, get_function_name("getSchema").c_str());
  shcore::Value ret_val;

  try {
    std::string type = "Schema";
    std::string search_name = args.string_at(0);
    std::string name = db_object_exists(type, search_name, "");

    if (!name.empty()) {
      update_schema_cache(name, true);

      ret_val = (*_schemas)[name];

      ret_val.as_object<Schema>()->update_cache();
    } else {
      update_schema_cache(search_name, false);

      throw Exception::runtime_error("Unknown database '" + search_name + "'");
    }
  }
  CATCH_AND_TRANSLATE_FUNCTION_EXCEPTION(get_function_name("getSchema"));

  return ret_val;
}

// Documentation of  function
REGISTER_HELP(BASESESSION_GETSCHEMAS_BRIEF, "Retrieves the Schemas available on the session.");
REGISTER_HELP(BASESESSION_GETSCHEMAS_RETURN, "@return A List containing the Schema objects available on the session.");
/**
* $(BASESESSION_GETSCHEMAS_BRIEF)
*
* $(BASESESSION_GETSCHEMAS_RETURN)
*/
#if DOXYGEN_JS
List BaseSession::getSchemas() {}
#elif DOXYGEN_PY
list BaseSession::get_schemas() {}
#endif
shcore::Value BaseSession::get_schemas(const shcore::Argument_list &args) const {
  args.ensure_count(0, get_function_name("getSchemas").c_str());

  shcore::Value::Array_type_ref schemas(new shcore::Value::Array_type);

  try {
    if (_session.is_connected()) {
      std::shared_ptr< ::mysqlx::Result> result = execute_sql("show databases;");
      std::shared_ptr< ::mysqlx::Row> row = result->next();

      while (row) {
        std::string schema_name;
        if (!row->isNullField(0))
          schema_name = row->stringField(0);

        if (!schema_name.empty()) {
          update_schema_cache(schema_name, true);

          schemas->push_back((*_schemas)[schema_name]);
        }

        row = result->next();
      }

      result->flush();
    }
  }
  CATCH_AND_TRANSLATE();

  return shcore::Value(schemas);
}

shcore::Value BaseSession::set_fetch_warnings(const shcore::Argument_list &args) {
  args.ensure_count(1, get_function_name("setFetchWarnings").c_str());

  bool enable = args.bool_at(0);
  std::string command = enable ? "enable_notices" : "disable_notices";

  shcore::Argument_list command_args;
  command_args.push_back(Value("warnings"));

  shcore::Value ret_val;
  try {
    ret_val = executeAdminCommand(command, false, command_args);
  }
  CATCH_AND_TRANSLATE_FUNCTION_EXCEPTION(get_function_name("setFetchWarnings"));

  return ret_val;
}

// Documentation of dropSchema function
REGISTER_HELP(BASESESSION_DROPSCHEMA_BRIEF, "Drops the schema with the specified name.");
REGISTER_HELP(BASESESSION_DROPSCHEMA_RETURN, "@return A SqlResult object if succeeded.");
REGISTER_HELP(BASESESSION_DROPSCHEMA_EXCEPTION, "@exception An error is raised if the schema did not exist.");

/**
* $(BASESESSION_DROPSCHEMA_BRIEF)
*
* $(BASESESSION_DROPSCHEMA_RETURN)
*
* $(BASESESSION_DROPSCHEMA_EXCEPTION)
*/
#if DOXYGEN_JS
Result BaseSession::dropSchema(String name) {}
#elif DOXYGEN_PY
Result BaseSession::drop_schema(str name) {}
#endif
shcore::Value BaseSession::drop_schema(const shcore::Argument_list &args) {
  std::string function = get_function_name("dropSchema");

  args.ensure_count(1, function.c_str());

  if (args[0].type != shcore::String)
    throw shcore::Exception::argument_error(function + ": Argument #1 is expected to be a string");

  std::string name = args[0].as_string();

  shcore::Value ret_val;
  try {
    ret_val = executeStmt("sql", sqlstring("drop schema !", 0) << name, false, shcore::Argument_list());

    if (_schemas->find(name) != _schemas->end())
      _schemas->erase(name);
  }
  CATCH_AND_TRANSLATE_FUNCTION_EXCEPTION(get_function_name("dropSchema"));

  return ret_val;
}

// Documentation of dropTable function
REGISTER_HELP(BASESESSION_DROPTABLE_BRIEF, "Drops a table from the specified schema.");
REGISTER_HELP(BASESESSION_DROPTABLE_RETURN, "@return A SqlResult object if succeeded.");
REGISTER_HELP(BASESESSION_DROPTABLE_EXCEPTION, "@exception An error is raised if the table did not exist.");

/**
* $(BASESESSION_DROPTABLE_BRIEF)
*
* $(BASESESSION_DROPTABLE_RETURN)
*
* $(BASESESSION_DROPTABLE_EXCEPTION)
*/
#if DOXYGEN_JS
Result BaseSession::dropTable(String schema, String name) {}
#elif DOXYGEN_PY
Result BaseSession::drop_table(str schema, str name) {}
#endif

// Documentation of dropCollection function
REGISTER_HELP(BASESESSION_DROPCOLLECTION_BRIEF, "Drops a collection from the specified schema.");
REGISTER_HELP(BASESESSION_DROPCOLLECTION_RETURN, "@return A SqlResult object if succeeded.");
REGISTER_HELP(BASESESSION_DROPCOLLECTION_EXCEPTION, "@exception An error is raised if the collection did not exist.");

/**
* $(BASESESSION_DROPCOLLECTION_BRIEF)
*
* $(BASESESSION_DROPCOLLECTION_RETURN)
*
* $(BASESESSION_DROPCOLLECTION_EXCEPTION)
*/
#if DOXYGEN_JS
Result BaseSession::dropCollection(String schema, String name) {}
#elif DOXYGEN_PY
Result BaseSession::drop_collection(str schema, str name) {}
#endif

// Documentation of dropView function
REGISTER_HELP(BASESESSION_DROPVIEW_BRIEF, "Drops a view from the specified schema.");
REGISTER_HELP(BASESESSION_DROPVIEW_RETURN, "@return A SqlResult object if succeeded.");
REGISTER_HELP(BASESESSION_DROPVIEW_EXCEPTION, "@exception An error is raised if the view did not exist.");

/**
* $(BASESESSION_DROPVIEW_BRIEF)
*
* $(BASESESSION_DROPVIEW_RETURN)
*
* $(BASESESSION_DROPVIEW_EXCEPTION)
*/
#if DOXYGEN_JS
Result BaseSession::dropView(String schema, String name) {}
#elif DOXYGEN_PY
Result BaseSession::drop_view(str schema, str name) {}
#endif

shcore::Value BaseSession::drop_schema_object(const shcore::Argument_list &args, const std::string& type) {
  std::string function = get_function_name("drop" + type);

  args.ensure_count(2, function.c_str());

  if (args[0].type != shcore::String)
    throw shcore::Exception::argument_error(function + ": Argument #1 is expected to be a string");

  if (args[1].type != shcore::String)
    throw shcore::Exception::argument_error(function + ": Argument #2 is expected to be a string");

  std::string schema = args[0].as_string();
  std::string name = args[1].as_string();

  shcore::Value ret_val;

  try {
    if (type == "View")
      ret_val = executeStmt("sql", sqlstring("drop view !.!", 0) << schema << name + "", false, shcore::Argument_list());
    else {
      shcore::Argument_list command_args;
      command_args.push_back(Value(schema));
      command_args.push_back(Value(name));

      ret_val = executeAdminCommand("drop_collection", false, command_args);
    }

    if (_schemas->count(schema)) {
      std::shared_ptr<Schema> schema_obj = std::static_pointer_cast<Schema>((*_schemas)[schema].as_object());
      if (schema_obj)
        schema_obj->_remove_object(name, type);
    }
  }
  CATCH_AND_TRANSLATE_FUNCTION_EXCEPTION(get_function_name("drop" + type));

  return ret_val;
}

/*
* This function verifies if the given object exist in the database, works for schemas, tables, views and collections.
* The check for tables, views and collections is done is done based on the type.
* If type is not specified and an object with the name is found, the type will be returned.
*
* Returns the name of the object as exists in the database.
*/
std::string BaseSession::db_object_exists(std::string &type, const std::string &name, const std::string& owner) const {
  return _session.db_object_exists(type, name, owner);
}

shcore::Value BaseSession::get_capability(const std::string& name) {
  return _session.get_capability(name);
}

shcore::Value BaseSession::get_status(const shcore::Argument_list &args) {
  shcore::Value::Map_type_ref status(new shcore::Value::Map_type);

  if (class_name() == "XSession")
    (*status)["SESSION_TYPE"] = shcore::Value("X");
  else
    (*status)["SESSION_TYPE"] = shcore::Value("Node");

  shcore::Value node_type = get_capability("node_type");
  if (node_type)
    (*status)["NODE_TYPE"] = node_type;

  (*status)["DEFAULT_SCHEMA"] = shcore::Value(_default_schema);

  std::shared_ptr< ::mysqlx::Result> result;
  std::shared_ptr< ::mysqlx::Row>row;

  try {
    result = execute_sql("select DATABASE(), USER() limit 1");
    row = result->next();

    std::string current_schema = row->isNullField(0) ? "" : row->stringField(0);
    if (current_schema == "null")
      current_schema = "";

    (*status)["CURRENT_SCHEMA"] = shcore::Value(current_schema);
    (*status)["CURRENT_USER"] = shcore::Value(row->isNullField(1) ? "" : row->stringField(1));
    (*status)["CONNECTION_ID"] = shcore::Value(_session.get_client_id());
    //(*status)["SSL_CIPHER"] = shcore::Value(_conn->get_ssl_cipher());
    //(*status)["SKIP_UPDATES"] = shcore::Value(???);
    //(*status)["DELIMITER"] = shcore::Value(???);

    //(*status)["SERVER_INFO"] = shcore::Value(_conn->get_server_info());

    //(*status)["PROTOCOL_VERSION"] = shcore::Value(_conn->get_protocol_info());
    //(*status)["CONNECTION"] = shcore::Value(_conn->get_connection_info());
    //(*status)["INSERT_ID"] = shcore::Value(???);

    result = execute_sql("select @@character_set_client, @@character_set_connection, @@character_set_server, @@character_set_database, @@version_comment limit 1");
    row = result->next();
    (*status)["CLIENT_CHARSET"] = shcore::Value(row->isNullField(0) ? "" : row->stringField(0));
    (*status)["CONNECTION_CHARSET"] = shcore::Value(row->isNullField(1) ? "" : row->stringField(1));
    (*status)["SERVER_CHARSET"] = shcore::Value(row->isNullField(2) ? "" : row->stringField(2));
    (*status)["SCHEMA_CHARSET"] = shcore::Value(row->isNullField(3) ? "" : row->stringField(3));
    (*status)["SERVER_VERSION"] = shcore::Value(row->isNullField(4) ? "" : row->stringField(4));

    //(*status)["SERVER_STATS"] = shcore::Value(_conn->get_stats());

    // TODO: Review retrieval from charset_info, mysql connection

    // TODO: Embedded library stuff
    //(*status)["TCP_PORT"] = row->get_value(1);
    //(*status)["UNIX_SOCKET"] = row->get_value(2);
    //(*status)["PROTOCOL_COMPRESSED"] = row->get_value(3);

    // STATUS

    // SAFE UPDATES
  } catch (shcore::Exception &e) {
    (*status)["STATUS_ERROR"] = shcore::Value(e.format());
  }

  return shcore::Value(status);
}

std::shared_ptr<BaseSession> XSession::_get_shared_this() const {
  std::shared_ptr<const XSession> shared = shared_from_this();

  return std::const_pointer_cast<XSession>(shared);
}

std::shared_ptr<shcore::Object_bridge> XSession::create(const shcore::Argument_list &args) {
  return connect_session(args, mysqlsh::SessionType::X);
}

NodeSession::NodeSession() : BaseSession() {
  init();
}

NodeSession::NodeSession(const NodeSession& s) : BaseSession(s) {
  init();
}

void NodeSession::init() {
  add_property("currentSchema", "getCurrentSchema");

  add_method("sql", std::bind(&NodeSession::sql, this, _1), "sql", shcore::String, NULL);
  add_method("setCurrentSchema", std::bind(&NodeSession::set_current_schema, this, _1), "name", shcore::String, NULL);
  add_method("quoteName", std::bind(&NodeSession::quote_name, this, _1), "name", shcore::String, NULL);
}

std::shared_ptr<BaseSession> NodeSession::_get_shared_this() const {
  std::shared_ptr<const NodeSession> shared = shared_from_this();

  return std::const_pointer_cast<NodeSession>(shared);
}

std::shared_ptr<shcore::Object_bridge> NodeSession::create(const shcore::Argument_list &args) {
  return connect_session(args, mysqlsh::SessionType::Node);
}

// Documentation of sql function
REGISTER_HELP(BASESESSION_SQL_BRIEF, "Creates a SqlExecute object to allow running the received SQL statement on the target MySQL Server.");
REGISTER_HELP(BASESESSION_SQL_PARAM, "@param sql A string containing the SQL statement to be executed.");
REGISTER_HELP(BASESESSION_SQL_RETURN, "@return A SqlExecute object.");
REGISTER_HELP(BASESESSION_SQL_DETAIL, "This method creates an SqlExecute object which is a SQL execution handler.");
REGISTER_HELP(BASESESSION_SQL_DETAIL1, "The SqlExecute class has functions that allow defining the way the statement will be executed "\
"and allows doing parameter binding.");
REGISTER_HELP(BASESESSION_SQL_DETAIL2, "The received SQL is set on the execution handler.");

/**
* $(BASESESSION_SQL_BRIEF)
*
* $(BASESESSION_SQL_PARAM)
*
* $(BASESESSION_SQL_RETURN)
*
* $(BASESESSION_SQL_DETAIL)
*
* $(BASESESSION_SQL_DETAIL1)
*
* $(BASESESSION_SQL_DETAIL2)
*
* JavaScript Example
* \code{.js}
* var sql = session.sql("select * from mydb.students where  age > ?");
* var result = sql.bind(18).execute();
* \endcode
* \sa SqlExecute
*/
#if DOXYGEN_JS
SqlExecute NodeSession::sql(String sql) {}
#elif DOXYGEN_PY
SqlExecute NodeSession::sql(str sql) {}
#endif
shcore::Value NodeSession::sql(const shcore::Argument_list &args) {
  std::shared_ptr<SqlExecute> sql_execute(new SqlExecute(shared_from_this()));

  return sql_execute->sql(args);
}

// Documentation of getCurrentSchema function
REGISTER_HELP(BASESESSION_GETCURRENTSCHEMA_BRIEF, "Retrieves the Schema set as active on the session.");
REGISTER_HELP(BASESESSION_GETCURRENTSCHEMA_RETURN, "@return A Schema object or Null");

/**
* $(BASESESSION_GETCURRENTSCHEMA_BRIEF)
*
* $(BASESESSION_GETCURRENTSCHEMA_RETURN)
*/
#if DOXYGEN_JS
Schema NodeSession::getCurrentSchema() {}
#elif DOXYGEN_PY
Schema NodeSession::get_current_schema() {}
#endif

Value NodeSession::get_member(const std::string &prop) const {
  Value ret_val;

  if (prop == "currentSchema") {
    NodeSession *session = const_cast<NodeSession *>(this);
    std::string name = session->_retrieve_current_schema();

    if (!name.empty()) {
      shcore::Argument_list args;
      args.push_back(shcore::Value(name));
      ret_val = get_schema(args);
    } else
      ret_val = Value::Null();
  } else
  ret_val = BaseSession::get_member(prop);

  return ret_val;
}

// Documentation of quoteName function
REGISTER_HELP(BASESESSION_QUOTENAME_BRIEF, "Escapes the passed identifier.");
REGISTER_HELP(BASESESSION_QUOTENAME_RETURN, "@return A String containing the escaped identifier.");

/**
* $(BASESESSION_QUOTENAME_BRIEF)
*
* $(BASESESSION_QUOTENAME_RETURN)
*/
#if DOXYGEN_JS
String NodeSession::quoteName(String id) {}
#elif DOXYGEN_PY
str NodeSession::quote_name(str id) {}
#endif
shcore::Value NodeSession::quote_name(const shcore::Argument_list &args) {
  args.ensure_count(1, get_function_name("quoteName").c_str());

  if (args[0].type != shcore::String)
    throw shcore::Exception::type_error("Argument #1 is expected to be a string");

  std::string id = args[0].as_string();

  return shcore::Value(get_quoted_name(id));
}

// Documentation of setCurrentSchema function
REGISTER_HELP(BASESESSION_SETCURRENTSCHEMA_BRIEF, "Sets the current schema for this session, and returns the schema object for it.");
REGISTER_HELP(BASESESSION_SETCURRENTSCHEMA_PARAM, "@param name the name of the new schema to switch to.");
REGISTER_HELP(BASESESSION_SETCURRENTSCHEMA_RETURN, "@return the Schema object for the new schema.");
REGISTER_HELP(BASESESSION_SETCURRENTSCHEMA_DETAIL, "At the database level, this is equivalent at issuing the following SQL query:");
REGISTER_HELP(BASESESSION_SETCURRENTSCHEMA_DETAIL1, "  use <new-default-schema>;");

/**
* $(BASESESSION_SETCURRENTSCHEMA_BRIEF)
*
* $(BASESESSION_SETCURRENTSCHEMA_PARAM)
* $(BASESESSION_SETCURRENTSCHEMA_RETURN)
*
* $(BASESESSION_SETCURRENTSCHEMA_DETAIL)
* $(BASESESSION_SETCURRENTSCHEMA_DETAIL1)
*/
#if DOXYGEN_JS
Schema NodeSession::setCurrentSchema(String name) {}
#elif DOXYGEN_PY
Schema NodeSession::set_current_schema(str name) {}
#endif

shcore::Value NodeSession::set_current_schema(const shcore::Argument_list &args) {
  args.ensure_count(1, get_function_name("setCurrentSchema").c_str());

  if (_session.is_connected()) {
    std::string name = args[0].as_string();

    std::shared_ptr< ::mysqlx::Result> result = execute_sql(sqlstring("use !", 0) << name);
    result->flush();
  } else
  throw Exception::runtime_error(class_name() + " not connected");

  return get_member("currentSchema");
}
