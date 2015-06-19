/*
 * Copyright (c) 2014, Oracle and/or its affiliates. All rights reserved.
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

#include "proj_parser.h"
#include <boost/bind.hpp>
#include <boost/algorithm/string.hpp>

#include "crud_definition.h"

using namespace mysh;
using namespace mysh::mysqlx;
using namespace shcore;

Crud_definition::Crud_definition()
{
  try
  {
    add_method("execute", boost::bind(&Crud_definition::execute, this, _1), "data");
  }
  catch (shcore::Exception &e)
  {
    // Invalid typecast exception is the only option
    // The exception is recreated with a more explicit message
    throw shcore::Exception::argument_error("Invalid connection used on CRUD operation.");
  }
}

std::vector<std::string> Crud_definition::get_members() const
{
  std::vector<std::string> _members;
  for (std::map<std::string, boost::shared_ptr<shcore::Cpp_function> >::const_iterator i = _funcs.begin(); i != _funcs.end(); ++i)
  {
    // Only returns the enabled functions
    if (_enabled_functions.at(i->first))
      _members.push_back(i->first);
  }
  return _members;
}

Value Crud_definition::get_member(const std::string &prop) const
{
  std::map<std::string, boost::shared_ptr<shcore::Cpp_function> >::const_iterator i;
  if ((i = _funcs.find(prop)) == _funcs.end())
    throw shcore::Exception::attrib_error("Invalid object member " + prop);
  else if (!_enabled_functions.at(prop))
    throw shcore::Exception::logic_error("Forbidden usage of " + prop);
  else
    return Value(boost::shared_ptr<shcore::Function_base>(i->second));
}

bool Crud_definition::has_member(const std::string &prop) const
{
  std::map<std::string, boost::shared_ptr<shcore::Cpp_function> >::const_iterator i;
  return ((i = _funcs.find(prop)) != _funcs.end() && _enabled_functions.at(prop));
}

Value Crud_definition::call(const std::string &name, const shcore::Argument_list &args)
{
  std::map<std::string, boost::shared_ptr<shcore::Cpp_function> >::const_iterator i;
  if ((i = _funcs.find(name)) == _funcs.end() || !_enabled_functions.at(name))
    throw shcore::Exception::attrib_error("Invalid object function " + name);
  return i->second->invoke(args);
}

/*
* This method registers the "dynamic" behavior of the functions exposed by the object.
* Parameters:
*   - name: indicates the exposed function to be enabled/disabled.
*   - enable_after: indicate the "states" under which the function should be enabled.
*/
void Crud_definition::register_dynamic_function(const std::string& name, const std::string& enable_after)
{
  // Adds the function to the enabled/disabled state registry
  _enabled_functions[name] = true;

  // Splits the 'enable' states and associates them to the function
  std::vector<std::string> tokens;
  boost::algorithm::split(tokens, enable_after, boost::is_any_of(", "), boost::token_compress_on);
  std::set<std::string> after(tokens.begin(), tokens.end());
  _enable_paths[name] = after;
}

void Crud_definition::update_functions(const std::string& source)
{
  std::map<std::string, bool>::iterator it, end = _enabled_functions.end();

  for (it = _enabled_functions.begin(); it != end; it++)
  {
    size_t count = _enable_paths[it->first].count(source);
    enable_function(it->first.c_str(), count > 0);
  }
}

void Crud_definition::enable_function(const char *name, bool enable)
{
  if (_enabled_functions.find(name) != _enabled_functions.end())
    _enabled_functions[name] = enable;
}

shcore::Value Crud_definition::execute(const shcore::Argument_list &args)
{
  // TODO: Callback handling logic
  shcore::Value ret_val;
  /*boost::shared_ptr<mysh::X_connection> connection(_conn.lock());

  if (connection)
  ret_val = connection->crud_execute(class_name(), _data);*/

  return ret_val;
}

void Crud_definition::set_filter(const std::string &source, const std::string &field, shcore::Value value, bool collection)
{
  /*// First validates that the received value is a string
  if (value.type != shcore::String)
  throw shcore::Exception::argument_error(source + ": string parameter required.");

  // Now validates that it is a valid expression
  try
  {
  // This basically validates the expression is correct, if an error is found
  // a runtime exception will be generated by the parser
  mysqlx::Expr_parser parser(value.as_string(), collection);
  parser.expr();

  (*_data)[field] = value;
  }
  catch (mysqlx::Parser_error &e)
  {
  std::string error = source;
  error.append(": ");
  error.append(e.what());
  throw shcore::Exception::argument_error(error);
  }*/
}

void Crud_definition::set_columns(const std::string &source, const std::string &field, shcore::Value value, bool collection, bool with_alias)
{
  /*// First validates that the received value is a string
  if (value.type != shcore::String)
  throw shcore::Exception::argument_error(source + ": string parameter required.");

  // Now validates that it is a valid expression
  try
  {
  // We use the parser directly for simplicity
  mysqlx::Proj_parser parser(value.as_string(), collection, with_alias);
  //parser.parse();
  //parser.projection();

  (*_data)[field] = value;
  }
  catch (std::runtime_error &e)
  {
  std::string error = source;
  error.append(": ");
  error.append(e.what());
  throw shcore::Exception::argument_error(error);
  }*/
}

void Crud_definition::set_order(const std::string &source, const std::string &field, shcore::Value value)
{
  /*// First validates that the received value is a string
  if (value.type != shcore::String)
  throw shcore::Exception::argument_error(source + ": string parameter required.");

  // TODO: remove this exception when the Order_parser is implemented
  throw shcore::Exception::logic_error(source + ": not yet implemented.");

  // Now validates that it is a valid expression
  try
  {
  // TODO: The projection parser should be called just to ensure the received data is correct
  //Order_parser order(value.as_string());

  //(*_data)[field] = value;
  }
  catch (std::runtime_error &e)
  {
  std::string error = source;
  error.append(": ");
  error.append(e.what());
  throw shcore::Exception::argument_error(error);
  }*/
}