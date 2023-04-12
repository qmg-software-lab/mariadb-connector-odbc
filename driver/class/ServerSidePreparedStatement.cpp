/************************************************************************************
   Copyright (C) 2022 MariaDB Corporation AB

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Library General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Library General Public License for more details.

   You should have received a copy of the GNU Library General Public
   License along with this library; if not see <http://www.gnu.org/licenses>
   or write to the Free Software Foundation, Inc.,
   51 Franklin St., Fifth Floor, Boston, MA 02110, USA
*************************************************************************************/


#include <deque>

#include "ServerSidePreparedStatement.h"
#include "Results.h"
//#include "ServerPrepareResult.h"
#include "ResultSetMetaData.h"
#include "ServerPrepareResult.h"
#include "interface/Exception.h"

namespace odbc
{
namespace mariadb
{
  ServerSidePreparedStatement::~ServerSidePreparedStatement()
  {
    if (results) {
      results->loadFully(false);
      results.reset();
    }
    serverPrepareResult.reset();
  }
  /**
    * Constructor for creating Server prepared statement.
    *
    * @param connection current connection
    * @param sql Sql String to prepare
    * @param resultSetScrollType one of the following <code>ResultSet</code> constants: <code>
    *     ResultSet.SQL_CURSOR_FORWARD_ONLY</code>, <code>ResultSet.TYPE_SCROLL_INSENSITIVE</code>, or
    *     <code>ResultSet.TYPE_SCROLL_SENSITIVE</code>
    * @param resultSetConcurrency a concurrency type; one of <code>ResultSet.CONCUR_READ_ONLY</code>
    *     or <code>ResultSet.CONCUR_UPDATABLE</code>
    * @param autoGeneratedKeys a flag indicating whether auto-generated keys should be returned; one
    *     of <code>Statement.RETURN_GENERATED_KEYS</code> or <code>Statement.NO_GENERATED_KEYS</code>
    * @throws SQLException exception
    */
  ServerSidePreparedStatement::ServerSidePreparedStatement(
    MYSQL* _connection, const SQLString& _sql,
    int32_t resultSetScrollType)
    : ServerSidePreparedStatement(_connection, resultSetScrollType)
  {
    sql= _sql;
    prepare(sql);
  }

  ServerSidePreparedStatement::ServerSidePreparedStatement(
    MYSQL* _connection,
    int32_t resultSetScrollType)
    : PreparedStatement(_connection, resultSetScrollType),
      serverPrepareResult(nullptr)
  {
  }

  /**
    * Clone statement.
    *
    * @param connection connection
    * @return Clone statement.
    * @throws CloneNotSupportedException if any error occur.
    */
  ServerSidePreparedStatement* ServerSidePreparedStatement::clone(MYSQL* connection)
  {
    ServerSidePreparedStatement* clone= new ServerSidePreparedStatement(connection, this->resultSetScrollType);
    clone->metadata.reset(new ResultSetMetaData(*metadata));
    clone->prepare(sql);

    return clone;
  }


  void ServerSidePreparedStatement::prepare(const SQLString& sql)
  {
    int32_t rc= 1;
    MYSQL_STMT* stmtId = mysql_stmt_init(connection);
    if (stmtId == nullptr) {
      throw rc;
    }
    static const my_bool updateMaxLength = 1;
    
    mysql_stmt_attr_set(stmtId, STMT_ATTR_UPDATE_MAX_LENGTH, &updateMaxLength);

    if ( (rc= mysql_stmt_prepare(stmtId, sql.c_str(), static_cast<unsigned long>(sql.length()))) != 0) {
      SQLException e(mysql_stmt_error(stmtId), mysql_stmt_sqlstate(stmtId), mysql_stmt_errno(stmtId));
      mysql_stmt_close(stmtId);
      throw e;
    }
    serverPrepareResult.reset(new ServerPrepareResult(sql, stmtId, connection));
    setMetaFromResult();

  }

  void ServerSidePreparedStatement::setMetaFromResult()
  {
    parameterCount= static_cast<int32_t>(serverPrepareResult->getParamCount());
    //initParamset(parameterCount);
    metadata.reset(serverPrepareResult->getEarlyMetaData());
    //parameterMetaData.reset(new MariaDbParameterMetaData(serverPrepareResult->getParameters()));
  }

  /* void ServerSidePreparedStatement::setParameter(int32_t parameterIndex, ParameterHolder* holder)
  {
    // TODO: does it really has to be map? can be, actually
    if (parameterIndex > 0 && parameterIndex < serverPrepareResult->getParamCount() + 1) {
      parameters[parameterIndex - 1].reset(holder);
    }
    else {
      SQLString error("Could not set parameter at position ");

      error.append(std::to_string(parameterIndex)).append(" (values was ").append(holder->toString()).append(")\nQuery - conn:");

      // A bit ugly - index validity is checked after parameter holder objects have been created.
      delete holder;

      error.append(std::to_string(getServerThreadId())).append(protocol->isMasterConnection() ? "(M)" : "(S)");
      error.append(" - \"");

      int32_t maxQuerySizeToLog= protocol->getOptions()->maxQuerySizeToLog;
      if (maxQuerySizeToLog > 0) {
        if (sql.size() < maxQuerySizeToLog) {
          error.append(sql);
        }
        else {
          error.append(sql.substr(0, maxQuerySizeToLog - 3) + "...");
        }
      }
      else {
        error.append(sql);
      }
      error.append(" - \"");
      logger->error(error);
      ExceptionFactory::INSTANCE.create(error).Throw();
    }
  }


  ParameterMetaData* ServerSidePreparedStatement::getParameterMetaData()
  {
    if (isClosed()) {
      throw SQLException("The query has been already closed");
    }

    return new MariaDbParameterMetaData(*parameterMetaData);
  } */

  ResultSetMetaData* ServerSidePreparedStatement::getMetaData()
  {
    return metadata.release(); // or get() ?
  }


  void ServerSidePreparedStatement::executeBatchInternal(uint32_t queryParameterSize)
  {
    executeQueryPrologue(serverPrepareResult.get());

    results.reset(
      new Results(
        this,
        0,
        true,
        queryParameterSize,
        true,
        resultSetScrollType,
        emptyStr,
        nullptr));

    mysql_stmt_attr_set(serverPrepareResult->getStatementId(), STMT_ATTR_ARRAY_SIZE, (void*)&batchArraySize);
    if (param != nullptr) {
      mysql_stmt_bind_param(serverPrepareResult->getStatementId(), param);
    }
    int32_t rc= mysql_stmt_execute(serverPrepareResult->getStatementId());
    if ( rc == 0)
    {
      getResult();
      if (!metadata) {
        setMetaFromResult();
      }
      results->commandEnd();
      return;
    }
    else
    {
      throw rc;
    }
    clearBatch();
  }


  void ServerSidePreparedStatement::executeQueryPrologue(ServerPrepareResult* serverPrepareResult)
  {
    checkClose();
  }


  void ServerSidePreparedStatement::getResult()
  {
    if (fieldCount() == 0) {
      results->addStats(mysql_stmt_affected_rows(serverPrepareResult->getStatementId()), hasMoreResults());
    }
    else {
      serverPrepareResult->reReadColumnInfo();
      ResultSet *rs= ResultSet::create(results.get(), serverPrepareResult.get());
      results->addResultSet(rs, hasMoreResults() || results->getFetchSize() > 0);
    }
  }


  bool ServerSidePreparedStatement::executeInternal(int32_t fetchSize)
  {
    checkClose();
    validateParamset(serverPrepareResult->getParamCount());

    results.reset(
      new Results(
        this,
        fetchSize,
        false,
        1,
        true,
        resultSetScrollType,
        sql,
        param));

      
    int32_t rc= mysql_stmt_execute(serverPrepareResult->getStatementId());

    if (rc == 0) {
      getResult();
      results->commandEnd();
    }
    else {
      results->commandEnd();
      throw rc;
    }

    return results->getResultSet() != nullptr;
  }

  uint32_t ServerSidePreparedStatement::fieldCount() const
  {
    return mysql_stmt_field_count(serverPrepareResult->getStatementId());;
  }

  void ServerSidePreparedStatement::close()
  {
    if (closed) {
      return;
    }

    markClosed();
    if (results) {
      if (results->getFetchSize() != 0) {
        // Probably not current results, but current streamer's results
        results->loadFully(true);
      }
      results->close();
    }

    if (serverPrepareResult) {
      serverPrepareResult.reset(); //?
    }
  }

  const char* ServerSidePreparedStatement::getError()
  {
    return mysql_stmt_error(serverPrepareResult->getStatementId());
  }

  uint32_t ServerSidePreparedStatement::getErrno()
  {
    return mysql_stmt_errno(serverPrepareResult->getStatementId());
  }

  const char* ServerSidePreparedStatement::getSqlState()
  {
    return mysql_stmt_sqlstate(serverPrepareResult->getStatementId());
  }

  bool ServerSidePreparedStatement::bind(MYSQL_BIND* param)
  {
    this->param= param;
    return mysql_stmt_bind_param(serverPrepareResult->getStatementId(), param) != '\0';
  }

  bool ServerSidePreparedStatement::sendLongData(uint32_t paramNum, const char* data, std::size_t length)
  {
    return mysql_stmt_send_long_data(serverPrepareResult->getStatementId(), paramNum, data, static_cast<unsigned long>(length)) != '\0';
  }


  bool ServerSidePreparedStatement::hasMoreResults()
  {
    return mysql_stmt_more_results(serverPrepareResult->getStatementId());
  }


  void ServerSidePreparedStatement::moveToNextResult()
  {
    int rc = mysql_stmt_next_result(serverPrepareResult->getStatementId());
    if (rc) {
      throw rc;
    }
    getResult();
  }
}
}
