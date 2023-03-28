/************************************************************************************
   Copyright (C) 2013,2023 MariaDB Corporation AB
   
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
#include "ma_odbc.h"
#include "ServerSidePreparedStatement.h"
#include "ClientSidePreparedStatement.h"
#include "interface/ResultSet.h"
#include "ResultSetMetaData.h"
#include "interface/Exception.h"

#define MADB_MIN_QUERY_LEN 5

/* {{{ MADB_RealQuery - the caller is responsible for locking, as the caller may need to do some operations
       before it's ok to unlock. e.g. read results */
SQLRETURN MADB_RealQuery(MADB_Dbc* Dbc, char* StatementText, SQLINTEGER TextLength, MADB_Error* Error)
{
  SQLRETURN ret = SQL_ERROR;

  if (StatementText)
  {
    if (MADB_GOT_STREAMER(Dbc) && Dbc->Methods->CacheRestOfCurrentRsStream(Dbc, Error))
    {
      return Error->ReturnValue;
    }
    MDBUG_C_PRINT(Dbc, "mysql_real_query(%0x,%s,%lu)", Dbc->mariadb, StatementText, TextLength);
    if (!mysql_real_query(Dbc->mariadb, StatementText, TextLength))
    {
      ret = SQL_SUCCESS;
      MADB_CLEAR_ERROR(Error);

      Dbc->Methods->TrackSession(Dbc);
    }
    else
    {
      MADB_SetNativeError(Error, SQL_HANDLE_DBC, Dbc->mariadb);
    }
  }
  else
    MADB_SetError(Error, MADB_ERR_HY001, mysql_error(Dbc->mariadb),
      mysql_errno(Dbc->mariadb));

  return ret;
}
/* }}} */

/* {{{ MADB_ExecuteQuery */
SQLRETURN MADB_ExecuteQuery(MADB_Stmt* Stmt, char* StatementText, SQLINTEGER TextLength)
{
  LOCK_MARIADB(Stmt->Connection);
  if (SQL_SUCCEEDED(MADB_RealQuery(Stmt->Connection, StatementText, TextLength, &Stmt->Error)))
  {
    Stmt->AffectedRows = mysql_affected_rows(Stmt->Connection->mariadb);
    /*if (MADB_GOT_STREAMER(Dbc) && Dbc->Methods->CacheRestOfCurrentRsStream(Dbc, Error))
    {
      return Error->ReturnValue;
    }
    Dbc->Methods->TrackSession(Dbc);*/
  }
  UNLOCK_MARIADB(Stmt->Connection);
  return Stmt->Error.ReturnValue;
}
/* }}} */

/* {{{ MADB_StmtBulkOperations */
SQLRETURN MADB_StmtBulkOperations(MADB_Stmt *Stmt, SQLSMALLINT Operation)
{
  MADB_CLEAR_ERROR(&Stmt->Error);
  switch(Operation)
  {
  case SQL_ADD:
     return Stmt->Methods->SetPos(Stmt, 0, SQL_ADD, SQL_LOCK_NO_CHANGE, 0);
  default:
    return SQL_ERROR;
  }
}
/* }}} */

/* {{{ RemoveStmtRefFromDesc
       Helper function removing references to the stmt in the descriptor when explisitly allocated descriptor is substituted
       by some other descriptor */
void RemoveStmtRefFromDesc(MADB_Desc *desc, MADB_Stmt *Stmt, BOOL all)
{
  if (desc->AppType)
  {
    unsigned int i;
    for (i=0; i < desc->Stmts.elements; ++i)
    {
      MADB_Stmt **refStmt= ((MADB_Stmt **)desc->Stmts.buffer) + i;
      if (Stmt == *refStmt)
      {
        MADB_DeleteDynamicElement(&desc->Stmts, i);

        if (!all)
        {
          return;
        }
      }
    }
  }
}
/* }}} */

/* {{{ MADB_StmtFree */
SQLRETURN MADB_StmtFree(MADB_Stmt *Stmt, SQLUSMALLINT Option)
{
  if (!Stmt)
    return SQL_INVALID_HANDLE;

  switch (Option) {
  case SQL_CLOSE:
    if (Stmt->stmt)
    {
      if (Stmt->Ird)
        MADB_DescFree(Stmt->Ird, TRUE);
      if (Stmt->State > MADB_SS_PREPARED)
      {
        MDBUG_C_PRINT(Stmt->Connection, "Closing resultset", Stmt->stmt.get());
        LOCK_MARIADB(Stmt->Connection);
        Stmt->rs.reset();
        while (Stmt->stmt->getMoreResults() || Stmt->stmt->getUpdateCount() != -1);

        UNLOCK_MARIADB(Stmt->Connection);
      }

      Stmt->metadata.reset();
     
      MADB_FREE(Stmt->result);
      MADB_FREE(Stmt->CharOffset);
      MADB_FREE(Stmt->Lengths);

      RESET_STMT_STATE(Stmt);
      RESET_DAE_STATUS(Stmt);
    }
    break;

  case SQL_UNBIND:
    MADB_FREE(Stmt->result);
    MADB_DescFree(Stmt->Ard, TRUE);
    break;

  case SQL_RESET_PARAMS:
    MADB_FREE(Stmt->params);
    MADB_DescFree(Stmt->Apd, TRUE);
    RESET_DAE_STATUS(Stmt);
    break;

  case SQL_DROP:
    MADB_FREE(Stmt->params);
    MADB_FREE(Stmt->result);
    MADB_FREE(Stmt->Cursor.Name);
    MADB_FREE(Stmt->CatalogName);
    MADB_FREE(Stmt->TableName);
    MADB_FREE(Stmt->UniqueIndex);
    //Stmt->metadata.reset();

    /* For explicit descriptors we only remove reference to the stmt*/
    if (Stmt->Apd->AppType)
    {
      EnterCriticalSection(&Stmt->Connection->ListsCs);
      RemoveStmtRefFromDesc(Stmt->Apd, Stmt, TRUE);
      LeaveCriticalSection(&Stmt->Connection->ListsCs);
      MADB_DescFree(Stmt->IApd, FALSE);
    }
    else
    {
      MADB_DescFree( Stmt->Apd, FALSE);
    }
    if (Stmt->Ard->AppType)
    {
      EnterCriticalSection(&Stmt->Connection->ListsCs);
      RemoveStmtRefFromDesc(Stmt->Ard, Stmt, TRUE);
      LeaveCriticalSection(&Stmt->Connection->ListsCs);
      MADB_DescFree(Stmt->IArd, FALSE);
    }
    else
    {
      MADB_DescFree(Stmt->Ard, FALSE);
    }
    MADB_DescFree(Stmt->Ipd, FALSE);
    MADB_DescFree(Stmt->Ird, FALSE);

    MADB_FREE(Stmt->CharOffset);
    MADB_FREE(Stmt->Lengths);
    //Stmt->DefaultsResult.reset();

    if (Stmt->DaeStmt != NULL)
    {
      Stmt->DaeStmt->Methods->StmtFree(Stmt->DaeStmt, SQL_DROP);
      Stmt->DaeStmt= NULL;
    }
    LOCK_MARIADB(Stmt->Connection);
    if (MADB_STMT_IS_STREAMING(Stmt))
    {
      MADB_RESET_STREAMER(Stmt->Connection);
    }

    if (Stmt->stmt != NULL)
    {
      MDBUG_C_PRINT(Stmt->Connection, "-->closing %0x", Stmt->stmt.get());
      MADB_STMT_CLOSE_STMT(Stmt);
    }
    /* Query has to be deleted after multistmt handles are closed, since the depends on info in the Query */
    UNLOCK_MARIADB(Stmt->Connection);
    EnterCriticalSection(&Stmt->Connection->ListsCs);
    Stmt->Connection->Stmts= MADB_ListDelete(Stmt->Connection->Stmts, &Stmt->ListItem);
    LeaveCriticalSection(&Stmt->Connection->ListsCs);
    
    delete Stmt;
  } /* End of switch (Option) */
  return SQL_SUCCESS;
}
/* }}} */

/* {{{ MADB_CheckIfExecDirectPossible
       Checking if we can deploy mariadb_stmt_execute_direct */
BOOL MADB_CheckIfExecDirectPossible(MADB_Stmt *Stmt)
{
  return MADB_ServerSupports(Stmt->Connection, MADB_CAPABLE_EXEC_DIRECT)
      && !(Stmt->Apd->Header.ArraySize > 1)                              /* With array of parameters exec_direct will be not optimal */
      && MADB_FindNextDaeParam(Stmt->Apd, -1, 1) == MADB_NOPARAM;
}
/* }}} */

/* {{{ MADB_BulkInsertPossible
       Checking if we can deploy mariadb_stmt_execute_direct */
bool MADB_BulkInsertPossible(MADB_Stmt *Stmt)
{
  return /* MADB_ServerSupports(Stmt->Connection, MADB_CAPABLE_PARAM_ARRAYS)
      && */(Stmt->Apd->Header.ArraySize > 1)
      && (Stmt->Apd->Header.BindType == SQL_PARAM_BIND_BY_COLUMN)       /* First we support column-wise binding */
      && (Stmt->Query.QueryType == MADB_QUERY_INSERT || Stmt->Query.QueryType == MADB_QUERY_UPDATE)
      && MADB_FindNextDaeParam(Stmt->Apd, -1, 1) == MADB_NOPARAM;       /* TODO: should be not very hard ot optimize to use bulk in this
                                                                        case for chunks of the array, delimitered by param rows with DAE
                                                                        In particular, MADB_FindNextDaeParam should consider Stmt->ArrayOffset */
}
/* }}} */
/* {{{ MADB_StmtExecDirect */
SQLRETURN MADB_StmtExecDirect(MADB_Stmt *Stmt, char *StatementText, SQLINTEGER TextLength)
{
  SQLRETURN ret;
  BOOL      ExecDirect= TRUE;

  ret= Stmt->Prepare(StatementText, TextLength, false);
  /* In case statement is not supported, we use mysql_query instead */
  if (!SQL_SUCCEEDED(ret))
  {
    /* Can't be really - we now run here CS prepare */
    //if ((Stmt->Error.NativeError == 1295/*ER_UNSUPPORTED_PS*/ ||
    //     Stmt->Error.NativeError == 1064/*ER_PARSE_ERROR*/))
    //{}
    //else
    return ret;
  }

  return Stmt->Methods->Execute(Stmt, ExecDirect);
}
/* }}} */

/* {{{ MADB_FindCursor */
MADB_Stmt *MADB_FindCursor(MADB_Stmt *Stmt, const char *CursorName)
{
  MADB_Dbc *Dbc= Stmt->Connection;
  MADB_List *LStmt, *LStmtNext;
  // TODO: mutex?
  for (LStmt= Dbc->Stmts; LStmt; LStmt= LStmtNext)
  {
    MADB_Cursor *Cursor= &((MADB_Stmt *)LStmt->data)->Cursor;
    LStmtNext= LStmt->next;

    if (Stmt != (MADB_Stmt *)LStmt->data &&
        Cursor->Name && _stricmp(Cursor->Name, CursorName) == 0)
    {
      return (MADB_Stmt *)LStmt->data;
    }
  }
  MADB_SetError(&Stmt->Error, MADB_ERR_34000, NULL, 0);
  return NULL;
}
/* }}} */

/* {{{ FetchMetadata */
ResultSetMetaData* FetchMetadata(MADB_Stmt *Stmt, bool early)
{
  /* TODO early probably is not needed here at all */
  if (early)
  {
    Stmt->metadata.reset(Stmt->stmt->getEarlyMetaData());
  }
  else
  {
    Stmt->metadata.reset(Stmt->rs->getMetaData());
  }
  return Stmt->metadata.get();
}
/* }}} */

/* {{{ MADB_StmtReset - reseting Stmt handler for new use. Has to be called inside a lock */
SQLRETURN MADB_StmtReset(MADB_Stmt* Stmt)
{
  if (Stmt->State > MADB_SS_PREPARED)
  {
    MDBUG_C_PRINT(Stmt->Connection, "mysql_stmt_free_result(%0x)", Stmt->stmt.get());
    Stmt->rs.reset();
  }

  if (Stmt->State >= MADB_SS_PREPARED)
  {
    MADB_NewStmtHandle(Stmt);
  }

  switch (Stmt->State)
  {
  case MADB_SS_EXECUTED:
  case MADB_SS_OUTPARAMSFETCHED:

    MADB_FREE(Stmt->result);
    MADB_FREE(Stmt->CharOffset);
    MADB_FREE(Stmt->Lengths);
    RESET_DAE_STATUS(Stmt);

  case MADB_SS_PREPARED:
    Stmt->metadata.reset();

    Stmt->PositionedCursor= NULL;
    Stmt->Ird->Header.Count= 0;

  default:
    Stmt->PositionedCommand= 0;
    Stmt->State= MADB_SS_INITED;
    MADB_CLEAR_ERROR(&Stmt->Error);
    MADB_FREE(Stmt->UniqueIndex);
    MADB_FREE(Stmt->TableName);
  }
  return SQL_SUCCESS;
}
/* }}} */

/* {{{ MADB_CsPrepare - Method called if we do client side prepare */
SQLRETURN MADB_CsPrepare(MADB_Stmt *Stmt)
{
  Stmt->stmt.reset(new ClientSidePreparedStatement(Stmt->Connection->mariadb, STMT_STRING(Stmt), Stmt->Options.CursorType
    , Stmt->Query.NoBackslashEscape));
  if ((Stmt->ParamCount= static_cast<SQLSMALLINT>(Stmt->stmt->getParamCount())/* + (MADB_POSITIONED_COMMAND(Stmt) ? MADB_POS_COMM_IDX_FIELD_COUNT(Stmt) : 0)*/) != 0)
  {
    if (Stmt->params)
    {
      MADB_FREE(Stmt->params);
    }
    /* If we have "WHERE CURRENT OF", we will need bind additionaly parameters for each field in the index */
    Stmt->params= (MYSQL_BIND *)MADB_CALLOC(sizeof(MYSQL_BIND) * Stmt->ParamCount);
  }
  return SQL_SUCCESS;
}
/* }}} */

/* {{{ MADB_RegularPrepare - Method called from SQLPrepare in case it is SQLExecDirect and if !(server > 10.2)
   (i.e. we aren't going to do mariadb_stmt_exec_direct)
   The connection should be locked by the caller
 */
SQLRETURN MADB_RegularPrepare(MADB_Stmt *Stmt)
{
  MDBUG_C_PRINT(Stmt->Connection, "mysql_stmt_prepare(%0x,%s)", Stmt->stmt.get(), STMT_STRING(Stmt));

  if (MADB_GOT_STREAMER(Stmt->Connection) && Stmt->Connection->Methods->CacheRestOfCurrentRsStream(Stmt->Connection, &Stmt->Error))
  {
    return Stmt->Error.ReturnValue;
  }

  try
  {
    Stmt->stmt.reset(new ServerSidePreparedStatement(Stmt->Connection->mariadb, STMT_STRING(Stmt),
      Stmt->Options.CursorType));
  }
  catch (SQLException& e)
  {
    if (e.getErrorCode() == 1064 && Stmt->Query.BatchAllowed)
    {
      Stmt->stmt.reset(new ClientSidePreparedStatement(Stmt->Connection->mariadb, STMT_STRING(Stmt),
        Stmt->Options.CursorType, Stmt->Query.NoBackslashEscape));
    }
    else
    {
      ///* Need to save error first */
      MADB_FromException(Stmt->Error, e);
      ///* We need to close the stmt here, or it becomes unusable like in ODBC-21 */
      MDBUG_C_PRINT(Stmt->Connection, "mysql_stmt_close(%0x)", Stmt->stmt.get());
      UNLOCK_MARIADB(Stmt->Connection);
      return Stmt->Error.ReturnValue;
    }
  }
  catch (int)
  {
    Stmt->stmt.reset(new ClientSidePreparedStatement(Stmt->Connection->mariadb, STMT_STRING(Stmt),
        Stmt->Options.CursorType, Stmt->Query.NoBackslashEscape));
  }

  Stmt->State= MADB_SS_PREPARED;

  Stmt->metadata.reset(Stmt->stmt->getEarlyMetaData());
  /* If we have result returning query - fill descriptor records with metadata */
  if (Stmt->metadata && Stmt->metadata->getColumnCount() > 0)
  {
    MADB_DescSetIrdMetadata(Stmt, Stmt->metadata->getFields(), Stmt->metadata->getColumnCount());
  }

  if ((Stmt->ParamCount= (SQLSMALLINT)Stmt->stmt->getParamCount()) > 0)
  {
    if (Stmt->params)
    {
      MADB_FREE(Stmt->params);
    }
    Stmt->params= (MYSQL_BIND *)MADB_CALLOC(sizeof(MYSQL_BIND) * Stmt->ParamCount);
  }

  return SQL_SUCCESS;
}
/* }}} */

void MADB_AddQueryTime(MADB_QUERY* Query, unsigned long long Timeout)
{
  /* sizeof("SET STATEMENT max_statement_time= FOR ") = 38 */
  size_t NewSize= Query->Original.length() + 38 + 20/* max SQLULEN*/ + 1;
  SQLString query(Query->Original);
  Query->Original.reserve(NewSize);
  Query->Original.assign("SET STATEMENT max_statement_time=", sizeof("SET STATEMENT max_statement_time=") - 1);
  Query->Original.append(std::to_string(Timeout)).append(" FOR ").append(query);
}

/* {{{ MADB_StmtPrepare */
SQLRETURN MADB_Stmt::Prepare(char *StatementText, SQLINTEGER TextLength, bool ServerSide)
{
  const char   *CursorName= NULL;
  unsigned int  WhereOffset;
  BOOL          HasParameters= 0;

  MDBUG_C_PRINT(Connection, "%sMADB_StmtPrepare", "\t->");

  /* After this point we can't have SQL_NTS*/
  ADJUST_INTLENGTH(StatementText, TextLength);
  /* There is no need to send anything to the server to find out there is syntax error here */
  if (TextLength < MADB_MIN_QUERY_LEN)
  {
    return MADB_SetError(&Error, MADB_ERR_42000, NULL, 0);
  }

  LOCK_MARIADB(Connection);

  if (MADB_StmtReset(this) != SQL_SUCCESS)
  {
    UNLOCK_MARIADB(Connection);
    return Error.ReturnValue;
  }

  MADB_ResetParser(this, StatementText, TextLength);
  MADB_ParseQuery(&Query);

  if ((Query.QueryType == MADB_QUERY_INSERT || Query.QueryType == MADB_QUERY_UPDATE || Query.QueryType == MADB_QUERY_DELETE)
    && MADB_FindToken(&Query, "RETURNING"))
  {
    Query.ReturnsResult= '\1';
  }

  if (Query.QueryType == MADB_QUERY_CALL)
  {
    ServerSide= true;
  }
  /* if we have multiple statements we only prepare them on the client side */
  if (QueryIsPossiblyMultistmt(&Query) && QUERY_IS_MULTISTMT(Query))
  {
    if (Query.BatchAllowed)
    {
      MADB_CsPrepare(this);
      goto cleanandexit;
    }
    else
    {
      // If we think it's a multistatement, and they are not allowed, then the easiest way to return error is to prepare the query on server
      ServerSide= true;
    }
  }

  if (!MADB_ValidateStmt(&Query))
  {
    MADB_SetError(&Error, MADB_ERR_HY000, "SQL command SET NAMES is not allowed", 0);
    goto cleanandexit;
  }

  /* Transform WHERE CURRENT OF [cursorname]:
     Append WHERE with Parameter Markers
     In StmtExecute we will call SQLSetPos with update or delete:
     */

  if ((CursorName= MADB_ParseCursorName(&Query, &WhereOffset)))
  {
    MADB_DynString StmtStr;
    char *TableName;

    /* Make sure we have a delete or update statement
       MADB_QUERY_DELETE and MADB_QUERY_UPDATE defined in the enum to have the same value
       as SQL_UPDATE and SQL_DELETE, respectively */
    if (Query.QueryType == MADB_QUERY_DELETE || Query.QueryType == MADB_QUERY_UPDATE)
    {
      PositionedCommand= 1;
    }
    else
    {
      MADB_SetError(&Error, MADB_ERR_42000, "Invalid SQL Syntax: DELETE or UPDATE expected for positioned update", 0);
      goto cleanandexit;
    }

    if (!(PositionedCursor= MADB_FindCursor(this, CursorName)))
    {
      PositionedCommand= 0;
      goto cleanandexit;
    }

    /* If we don't cache RS of the referenced cursor now, we will still need to do this later, and if we can't now, we won't be able later.
       If there is other streamer - we can check later */
    if (MADB_STMT_IS_STREAMING(PositionedCursor) && Connection->Methods->CacheRestOfCurrentRsStream(Connection, &Error))
    {
      PositionedCommand= 0;
      PositionedCursor=  NULL;
      /* CacheRestOfCurrentRsStream methods sets Stmt's error*/
      goto cleanandexit;
    }

    TableName= MADB_GetTableName(PositionedCursor);
    MADB_InitDynamicString(&StmtStr, "", 8192, 1024);
    MADB_DynstrAppendMem(&StmtStr, Query.RefinedText.c_str(), WhereOffset);
    MADB_DynStrGetWhere(PositionedCursor, &StmtStr, TableName, TRUE);
    
    STMT_STRING(this).assign(StmtStr.str, StmtStr.length);
    /* Constructed query we've copied for execution has parameters */
    MADB_DynstrFree(&StmtStr);
  }

  if (Options.MaxRows)
  {
    /* TODO: LIMIT is not always the last clause. And not applicable to each query type.
       Thus we need to check query type and last tokens, and possibly put limit before them */
    STMT_STRING(this).reserve(STMT_STRING(this).length() + 32);
    STMT_STRING(this).append(" LIMIT ").append(std::to_string(Options.MaxRows));
  }

  if (Options.Timeout > 0)
  {
    MADB_AddQueryTime(&Query, Options.Timeout);
  }

  if (ServerSide)
  {
    MADB_RegularPrepare(this);
  }
  else
  {
    MADB_CsPrepare(this);
  }

cleanandexit:
  UNLOCK_MARIADB(Connection);
  return Error.ReturnValue;
}
/* }}} */

/* {{{ MADB_StmtParamData */ 
SQLRETURN MADB_StmtParamData(MADB_Stmt *Stmt, SQLPOINTER *ValuePtrPtr)
{
  MADB_Desc *Desc;
  MADB_DescRecord *Record;
  int ParamCount;
  int i;
  SQLRETURN ret;

  if (Stmt->DataExecutionType == MADB_DAE_NORMAL)
  {
    if (!Stmt->Apd || !(ParamCount= Stmt->ParamCount))
    {
      MADB_SetError(&Stmt->Error, MADB_ERR_HY010, NULL, 0);
      return Stmt->Error.ReturnValue;
    }
    Desc= Stmt->Apd;
  }
  else
  {
    if (!Stmt->Ard || !(ParamCount= Stmt->DaeStmt->ParamCount))
    {
      MADB_SetError(&Stmt->Error, MADB_ERR_HY010, NULL, 0);
      return Stmt->Error.ReturnValue;
    }
    Desc= Stmt->DaeStmt->Apd;
  }

  /* If we have last DAE param(Stmt->PutParam), we are starting from the next one. Otherwise from first */
  for (i= Stmt->PutParam > -1 ? Stmt->PutParam + 1 : 0; i < ParamCount; i++)
  {
    if ((Record= MADB_DescGetInternalRecord(Desc, i, MADB_DESC_READ)))
    {
      if (Record->OctetLengthPtr)
      {
        /* Stmt->DaeRowNumber is 1 based */
        SQLLEN *OctetLength = (SQLLEN *)GetBindOffset(Desc, Record, Record->OctetLengthPtr, Stmt->DaeRowNumber > 1 ? Stmt->DaeRowNumber - 1 : 0, sizeof(SQLLEN));
        if (PARAM_IS_DAE(OctetLength))
        {
          Stmt->PutDataRec= Record;
          *ValuePtrPtr = GetBindOffset(Desc, Record, Record->DataPtr, Stmt->DaeRowNumber > 1 ? Stmt->DaeRowNumber - 1 : 0, Record->OctetLength);
          Stmt->PutParam= i;
          Stmt->Status= SQL_NEED_DATA;

          return SQL_NEED_DATA;
        }
      }
    }
  }

  /* reset status, otherwise SQLSetPos and SQLExecute will fail */
  MARK_DAE_DONE(Stmt);
  if (Stmt->DataExecutionType == MADB_DAE_ADD || Stmt->DataExecutionType == MADB_DAE_UPDATE)
  {
    MARK_DAE_DONE(Stmt->DaeStmt);
  }

  switch (Stmt->DataExecutionType) {
  case MADB_DAE_NORMAL:
    ret= Stmt->Methods->Execute(Stmt, FALSE);
    RESET_DAE_STATUS(Stmt);
    break;
  case MADB_DAE_UPDATE:
    ret= Stmt->Methods->SetPos(Stmt, Stmt->DaeRowNumber, SQL_UPDATE, SQL_LOCK_NO_CHANGE, 1);
    RESET_DAE_STATUS(Stmt);
    break;
  case MADB_DAE_ADD:
    ret= Stmt->DaeStmt->Methods->Execute(Stmt->DaeStmt, FALSE);
    MADB_CopyError(&Stmt->Error, &Stmt->DaeStmt->Error);
    RESET_DAE_STATUS(Stmt->DaeStmt);
    break;
  default:
    ret= SQL_ERROR;
  }
  /* Interesting should we reset if execution failed? */

  return ret;
}
/* }}} */

/* {{{ MADB_StmtPutData */
SQLRETURN MADB_StmtPutData(MADB_Stmt *Stmt, SQLPOINTER DataPtr, SQLLEN StrLen_or_Ind)
{
  MADB_DescRecord *Record;
  MADB_Stmt       *MyStmt= Stmt;
  SQLPOINTER      ConvertedDataPtr= NULL;
  SQLULEN         Length= 0;

  MADB_CLEAR_ERROR(&Stmt->Error);

  if (DataPtr != NULL && StrLen_or_Ind < 0 && StrLen_or_Ind != SQL_NTS && StrLen_or_Ind != SQL_NULL_DATA)
  {
    MADB_SetError(&Stmt->Error, MADB_ERR_HY090, NULL, 0);
    return Stmt->Error.ReturnValue;
  }

  if (Stmt->DataExecutionType != MADB_DAE_NORMAL)
  {
    MyStmt= Stmt->DaeStmt;
  }
  Record= MADB_DescGetInternalRecord(MyStmt->Apd, Stmt->PutParam, MADB_DESC_READ);
  assert(Record);

  if (StrLen_or_Ind == SQL_NULL_DATA)
  {
    /* Check if we've already sent any data */
    if (false)// we should have some own flags for that, other than MyStmt->stmt->params[Stmt->PutParam].long_data_used)
    {
      MADB_SetError(&Stmt->Error, MADB_ERR_HY011, NULL, 0);
      return Stmt->Error.ReturnValue;
    }
    Record->Type= SQL_TYPE_NULL;
    return SQL_SUCCESS;
  }

  /* This normally should be enforced by DM */
  if (DataPtr == NULL && StrLen_or_Ind != 0)
  {
    MADB_SetError(&Stmt->Error, MADB_ERR_HY009, NULL, 0);
    return Stmt->Error.ReturnValue;
  }
/*
  if (StrLen_or_Ind == SQL_NTS)
  {
    if (Record->ConciseType == SQL_C_WCHAR)
      StrLen_or_Ind= wcslen((SQLWCHAR *)DataPtr);
    else
      StrLen_or_Ind= strlen((char *)DataPtr);
  }
 */
  if (Record->ConciseType == SQL_C_WCHAR)
  {
    /* Conn cs */
    ConvertedDataPtr= MADB_ConvertFromWChar((SQLWCHAR *)DataPtr, (SQLINTEGER)(StrLen_or_Ind/sizeof(SQLWCHAR)), &Length, &Stmt->Connection->Charset, NULL);

    if ((ConvertedDataPtr == NULL || Length == 0) && StrLen_or_Ind > 0)
    {
      MADB_SetError(&Stmt->Error, MADB_ERR_HY001, NULL, 0);
      return Stmt->Error.ReturnValue;
    }
  }
  else
  {
    if (StrLen_or_Ind == SQL_NTS)
    {
      Length= strlen((char *)DataPtr);
    }
    else
    {
      Length= StrLen_or_Ind;
    }
  }

  /* To make sure that we will not consume the doble amount of memory, we need to send
     data via mysql_send_long_data directly to the server instead of allocating a separate
     buffer. This means we need to process Update and Insert statements row by row. */
  if (MyStmt->stmt->sendLongData(Stmt->PutParam, static_cast<const char*>(ConvertedDataPtr ? ConvertedDataPtr : DataPtr), Length))
  {
    MADB_SetNativeError(&Stmt->Error, SQL_HANDLE_STMT, MyStmt->stmt.get());
  }
  else
  {
    Record->InternalLength+= (unsigned long)Length;
  }

  MADB_FREE(ConvertedDataPtr);
  return Stmt->Error.ReturnValue;
}
/* }}} */

/* {{{ MADB_ExecutePositionedUpdate */
SQLRETURN MADB_ExecutePositionedUpdate(MADB_Stmt *Stmt, BOOL ExecDirect)
{
  SQLSMALLINT   j, IndexIdx= 1;
  SQLRETURN     ret;
  MADB_DynArray DynData;
  MADB_Stmt     *SaveCursor;

  char *p;

  MADB_CLEAR_ERROR(&Stmt->Error);
  if (!Stmt->PositionedCursor->result)
  {
    MADB_SetError(&Stmt->Error, MADB_ERR_34000, "Cursor has no result set or is not open", 0);
    return Stmt->Error.ReturnValue;
  }
  MADB_StmtDataSeek(Stmt->PositionedCursor, Stmt->PositionedCursor->Cursor.Position);
  Stmt->Methods->RefreshRowPtrs(Stmt->PositionedCursor);

  memcpy(&Stmt->Apd->Header, &Stmt->Ard->Header, sizeof(MADB_Header));
  
  Stmt->AffectedRows= 0;
  
  MADB_InitDynamicArray(&DynData, sizeof(char *), 8, 8);

  for (j= 1; j < MADB_STMT_COLUMN_COUNT(Stmt->PositionedCursor) + 1; ++j)
  {
    if (Stmt->PositionedCursor->UniqueIndex == NULL ||
      (Stmt->PositionedCursor->UniqueIndex[0] != 0 && IndexIdx <= Stmt->PositionedCursor->UniqueIndex[0] && j == Stmt->PositionedCursor->UniqueIndex[IndexIdx] + 1))
    {
      SQLLEN Length;
      MADB_DescRecord* Rec= MADB_DescGetInternalRecord(Stmt->PositionedCursor->Ard, j, MADB_DESC_READ);
      SQLUSMALLINT ParamNumber= 0;

      Length= Rec->OctetLength;
      if (Stmt->PositionedCursor->UniqueIndex != NULL)
      {
        ParamNumber= /* Param ordnum in pos.cursor condition */IndexIdx +
                     /* Num of params in main stmt */(Stmt->ParamCount - Stmt->PositionedCursor->UniqueIndex[0]);
        ++IndexIdx;
      }
      else
      {
        ParamNumber = /* Param ordnum in pos.cursor condition */ j +
                      /* Num of params in main stmt */(Stmt->ParamCount - MADB_STMT_COLUMN_COUNT(Stmt->PositionedCursor));
      }
      /* if (Rec->inUse)
           MA_SQLBindParameter(Stmt, j+1, SQL_PARAM_INPUT, Rec->ConciseType, Rec->Type, Rec->DisplaySize, Rec->Scale, Rec->DataPtr, Length, Rec->OctetLengthPtr);
         else */
      {
        Stmt->Methods->GetData(Stmt->PositionedCursor, j, SQL_CHAR, NULL, 0, &Length, TRUE);
        p = (char*)MADB_CALLOC(Length + 2);
        MADB_InsertDynamic(&DynData, (char*)&p);
        Stmt->Methods->GetData(Stmt->PositionedCursor, j, SQL_CHAR, p, Length + 1, NULL, TRUE);
        Stmt->Methods->BindParam(Stmt, ParamNumber, SQL_PARAM_INPUT, SQL_CHAR, SQL_CHAR, 0, 0, p, Length, NULL);
      }
    }
  }

  SaveCursor= Stmt->PositionedCursor;
  Stmt->PositionedCursor= NULL;

  ret= Stmt->Methods->Execute(Stmt, ExecDirect);

  Stmt->PositionedCursor= SaveCursor;

  /* For the case of direct execution we need to restore number of parameters bound by application, for the case when application
     re-uses handle with same parameters for another query. Otherwise we won't know that number (of application's parameters) */
  if (ExecDirect)
  {
    Stmt->Apd->Header.Count-= MADB_POS_COMM_IDX_FIELD_COUNT(Stmt);
  }

  for (j=0; j < (int)DynData.elements; j++)
  {
    MADB_GetDynamic(&DynData, (char *)&p, j);
    MADB_FREE(p);
  }
  MADB_DeleteDynamic(&DynData);

  if (Stmt->PositionedCursor->Options.CursorType == SQL_CURSOR_DYNAMIC && 
     (ret == SQL_SUCCESS || ret == SQL_SUCCESS_WITH_INFO))
  {
    SQLRETURN rc;
    rc= Stmt->Methods->RefreshDynamicCursor(Stmt->PositionedCursor);
    if (!SQL_SUCCEEDED(rc))
    {
      MADB_CopyError(&Stmt->Error, &Stmt->PositionedCursor->Error);
      return Stmt->Error.ReturnValue;
    }
    if (Stmt->Query.QueryType == SQL_DELETE)
    {
      MADB_STMT_RESET_CURSOR(Stmt->PositionedCursor);
    }
      
  }
  //MADB_FREE(DataPtr);
  return ret;
}
/* }}} */

/* {{{ MADB_GetOutParams */
SQLRETURN MADB_Stmt::GetOutParams(int CurrentOffset)
{
  unsigned int i=0, ParameterNr= 0, columnCount= 0;
  
  try
  {
    metadata.reset(rs->getMetaData());
    columnCount= metadata->getColumnCount();
  }
  catch(int)
  {
    return MADB_SetNativeError(&Error, SQL_HANDLE_STMT, stmt.get());
  }

  MADB_FREE(result);
  result= (MYSQL_BIND *)MADB_CALLOC(sizeof(MYSQL_BIND) * columnCount);
  
  for (i=0; i < (unsigned int)ParamCount && ParameterNr < columnCount; ++i)
  {
    MADB_DescRecord *IpdRecord, *ApdRecord;
    if ((IpdRecord= MADB_DescGetInternalRecord(Ipd, i, MADB_DESC_READ))!= NULL)
    {
      if (IpdRecord->ParameterType == SQL_PARAM_INPUT_OUTPUT ||
          IpdRecord->ParameterType == SQL_PARAM_OUTPUT)
      {
        ApdRecord= MADB_DescGetInternalRecord(Apd, i, MADB_DESC_READ);
        result[ParameterNr].buffer= GetBindOffset(Apd, ApdRecord, ApdRecord->DataPtr, CurrentOffset, ApdRecord->OctetLength);
        if (ApdRecord->OctetLengthPtr)
        {
          result[ParameterNr].length= (unsigned long *)GetBindOffset(Apd, ApdRecord, ApdRecord->OctetLengthPtr,
                                                        CurrentOffset, sizeof(SQLLEN));
        }

        result[ParameterNr].buffer_type = MADB_GetMaDBTypeAndLength(ApdRecord->ConciseType, &result[ParameterNr].is_unsigned,
          &result[ParameterNr].buffer_length);
        result[ParameterNr].buffer_length= (unsigned long)ApdRecord->OctetLength;
        ++ParameterNr;
      }
    }
  }
  rs->bind(result);
  rs->first();
  rs->get();
  rs->beforeFirst();

  return SQL_SUCCESS;
}
/* }}} */

/* {{{ ResetInternalLength */
static void ResetInternalLength(MADB_Stmt *Stmt, unsigned int ParamOffset)
{
  unsigned int i;
  MADB_DescRecord *ApdRecord;

  for (i= ParamOffset; i < ParamOffset + Stmt->ParamCount; ++i)
  {
    if ((ApdRecord= MADB_DescGetInternalRecord(Stmt->Apd, i, MADB_DESC_READ)))
    {
      ApdRecord->InternalLength= 0;
    }
  }
}
/* }}} */

/* {{{ MADB_DoExecuteBatch */
/* Actually executing on the server, doing required actions with C API, and processing execution result */
SQLRETURN MADB_Stmt::DoExecuteBatch()
{
  SQLRETURN ret = SQL_SUCCESS;

  stmt->setBatchSize(Bulk.ArraySize);

  if (ParamCount)
  {
    stmt->bind(params);
  }
  try
  {
    const odbc::Longs &batchRes= stmt->executeBatch();
    rs.reset();
    AffectedRows+= stmt->getUpdateCount();
  }
  catch (int32_t /*rc*/)
  {
    MDBUG_C_PRINT(Connection, "execute:ERROR%s", "");
    return MADB_SetNativeError(&Error, SQL_HANDLE_STMT, stmt.get());
  }
  State = MADB_SS_EXECUTED;
  Connection->Methods->TrackSession(Connection);

  return ret;
}
/* }}} */

/* {{{ MADB_DoExecute */
/* Actually executing on the server, doing required actions with C API, and processing execution result */
SQLRETURN MADB_DoExecute(MADB_Stmt *Stmt)
{
  SQLRETURN ret= SQL_SUCCESS;

  Stmt->stmt->setBatchSize(Stmt->Bulk.ArraySize);

  if (Stmt->ParamCount)
  {
    Stmt->stmt->bind(Stmt->params);
  }
  try
  {
    if (Stmt->stmt->execute())
    {
      Stmt->rs.reset(Stmt->stmt->getResultSet());
    }
    else
    {
      Stmt->rs.reset();
      Stmt->AffectedRows+= Stmt->stmt->getUpdateCount();
    }
  }
  catch (int32_t /*rc*/)
  {
    MDBUG_C_PRINT(Stmt->Connection, "execute:ERROR%s", "");
    return MADB_SetNativeError(&Stmt->Error, SQL_HANDLE_STMT, Stmt->stmt.get());
  }

  unsigned int ServerStatus;

  Stmt->State= MADB_SS_EXECUTED;
  Stmt->Connection->Methods->TrackSession(Stmt->Connection);

  mariadb_get_infov(Stmt->Connection->mariadb, MARIADB_CONNECTION_SERVER_STATUS, (void*)&ServerStatus);
  if (ServerStatus & SERVER_PS_OUT_PARAMS)
  {
    Stmt->State= MADB_SS_OUTPARAMSFETCHED;
    ret= Stmt->GetOutParams(0);
  }
  return ret;
}
/* }}} */

void MADB_SetStatusArray(MADB_Stmt *Stmt, SQLUSMALLINT Status)
{
  if (Stmt->Ipd->Header.ArrayStatusPtr != NULL)
  {
    memset(Stmt->Ipd->Header.ArrayStatusPtr, 0x00ff & Status, Stmt->Apd->Header.ArraySize*sizeof(SQLUSMALLINT));
    if (Stmt->Apd->Header.ArrayStatusPtr != NULL)
    {
      unsigned int i;
      for (i= 0; i < Stmt->Apd->Header.ArraySize; ++i)
      {
        if (Stmt->Apd->Header.ArrayStatusPtr[i] == SQL_PARAM_IGNORE)
        {
          Stmt->Ipd->Header.ArrayStatusPtr[i]= SQL_PARAM_UNUSED;
        }
      }
    }
  }
}

/* For first row we just take its result as initial.
   For the rest, if all rows SQL_SUCCESS or SQL_ERROR - aggregated result is SQL_SUCCESS or SQL_ERROR, respectively
   Otherwise - SQL_SUCCESS_WITH_INFO */
#define CALC_ALL_ROWS_RC(_accumulated_rc, _cur_row_rc, _row_num)\
if      (_row_num == 0)                  _accumulated_rc= _cur_row_rc;\
else if (_cur_row_rc != _accumulated_rc) _accumulated_rc= SQL_SUCCESS_WITH_INFO

/* {{{ MADB_StmtExecute */
SQLRETURN MADB_StmtExecute(MADB_Stmt *Stmt, BOOL ExecDirect)
{
  unsigned int          i;
  MYSQL_RES   *DefaultResult= NULL;
  SQLRETURN    ret= SQL_SUCCESS, IntegralRc= SQL_SUCCESS;
  unsigned int ErrorCount=    0;
  unsigned int ParamOffset=   0; /* for multi statements */
               /* Will use it for STMT_ATTR_ARRAY_SIZE and as indicator if we are deploying MariaDB bulk insert feature */
  unsigned int MariadbArrSize= MADB_BulkInsertPossible(Stmt) ? (unsigned int)Stmt->Apd->Header.ArraySize : 0;
  SQLULEN      j, Start= Stmt->ArrayOffset;

  MDBUG_C_PRINT(Stmt->Connection, "%sMADB_StmtExecute", "\t->");

  MADB_CLEAR_ERROR(&Stmt->Error);

  if (MADB_POSITIONED_COMMAND(Stmt))
  {
    return MADB_ExecutePositionedUpdate(Stmt, ExecDirect);
  }

  /* Stmt->params was allocated during prepare, but could be cleared
     by SQLResetStmt. In latter case we need to allocate it again */
  if (!Stmt->params &&
    !(Stmt->params = (MYSQL_BIND *)MADB_CALLOC(sizeof(MYSQL_BIND) * MADB_STMT_PARAM_COUNT(Stmt))))
  {
    return MADB_SetError(&Stmt->Error, MADB_ERR_HY001, NULL, 0);
  }

  /* Normally this check is done by a DM. We are doing that too, keeping in mind direct linking.
     If exectution routine called from the SQLParamData, DataExecutionType has been reset */
  if (Stmt->Status == SQL_NEED_DATA && !DAE_DONE(Stmt))
  {
    MADB_SetError(&Stmt->Error, MADB_ERR_HY010, NULL, 0);
  }

  LOCK_MARIADB(Stmt->Connection);
  /* Prepare routine has the same check, thus I am not sure if we actually able to hit this case here */
  if (MADB_GOT_STREAMER(Stmt->Connection) && Stmt->Connection->Methods->CacheRestOfCurrentRsStream(Stmt->Connection, &Stmt->Error))
  {
    return Stmt->Error.ReturnValue;
  }

  Stmt->AffectedRows= 0;

  if (Stmt->Ipd->Header.RowsProcessedPtr)
  {
    *Stmt->Ipd->Header.RowsProcessedPtr= 0;
  }
 
  if (MariadbArrSize > 1)
  {
    if (MADB_DOING_BULK_OPER(Stmt))
    {
      //MADB_CleanBulkOperationData(Stmt);
    }
    Stmt->Bulk.ArraySize=  MariadbArrSize;
    Stmt->Bulk.HasRowsToSkip= 0;
  }

  if (MADB_DOING_BULK_OPER(Stmt))
  {
    if (!SQL_SUCCEEDED(MADB_ExecuteBulk(Stmt, ParamOffset)))
    {
      /* Doing just the same thing as we would do in general case */
      MADB_CleanBulkOperData(Stmt, ParamOffset);
      ErrorCount= (unsigned int)Stmt->Apd->Header.ArraySize;
      MADB_SetStatusArray(Stmt, SQL_PARAM_DIAG_UNAVAILABLE);
      goto end;
    }
    else if (!Stmt->rs)
    {
      Stmt->AffectedRows+= Stmt->stmt->getUpdateCount();
    }
    /* Suboptimal, but more reliable and simple */
    MADB_CleanBulkOperData(Stmt, ParamOffset);
    Stmt->ArrayOffset+= (int)Stmt->Apd->Header.ArraySize;
    if (Stmt->Ipd->Header.RowsProcessedPtr)
    {
      *Stmt->Ipd->Header.RowsProcessedPtr= *Stmt->Ipd->Header.RowsProcessedPtr + Stmt->Apd->Header.ArraySize;
    }
    MADB_SetStatusArray(Stmt, SQL_PARAM_SUCCESS);
  }
  else
  {
    /* Convert and bind parameters */
    for (j= Start; j < Start + Stmt->Apd->Header.ArraySize; ++j)
    {
      /* "... In an IPD, this SQLUINTEGER * header field points to a buffer containing the number
          of sets of parameters that have been processed, including error sets. ..." */
      if (Stmt->Ipd->Header.RowsProcessedPtr)
      {
        *Stmt->Ipd->Header.RowsProcessedPtr= *Stmt->Ipd->Header.RowsProcessedPtr + 1;
      }

      if (Stmt->Apd->Header.ArrayStatusPtr &&
        Stmt->Apd->Header.ArrayStatusPtr[j - Start] == SQL_PARAM_IGNORE)
      {
        if (Stmt->Ipd->Header.ArrayStatusPtr)
        {
          Stmt->Ipd->Header.ArrayStatusPtr[j - Start]= SQL_PARAM_UNUSED;
        }
        continue;
      }

      for (i= ParamOffset; i < ParamOffset + MADB_STMT_PARAM_COUNT(Stmt); ++i)
      {
        MADB_DescRecord *ApdRecord, *IpdRecord;

        if ((ApdRecord= MADB_DescGetInternalRecord(Stmt->Apd, i, MADB_DESC_READ)) &&
          (IpdRecord= MADB_DescGetInternalRecord(Stmt->Ipd, i, MADB_DESC_READ)))
        {
          /* check if parameter was bound */
          if (!ApdRecord->inUse)
          {
            IntegralRc= MADB_SetError(&Stmt->Error, MADB_ERR_07002, NULL, 0);
            goto end;
          }

          if (MADB_ConversionSupported(ApdRecord, IpdRecord) == FALSE)
          {
            IntegralRc= MADB_SetError(&Stmt->Error, MADB_ERR_07006, NULL, 0);
            goto end;
          }

          Stmt->params[i-ParamOffset].length= NULL;

          ret= MADB_C2SQL(Stmt, ApdRecord, IpdRecord, j - Start, &Stmt->params[i-ParamOffset]);
          if (!SQL_SUCCEEDED(ret))
          {
            if (ret == SQL_NEED_DATA)
            {
              IntegralRc= ret;
              ErrorCount= 0;
            }
            else
            {
              ++ErrorCount;
            }
            goto end;
          }
          CALC_ALL_ROWS_RC(IntegralRc, ret, j - Start);
        }
      }                 /* End of for() on parameters */

      if (Stmt->RebindParams && MADB_STMT_PARAM_COUNT(Stmt))
      {
        // TODO: was it really needed
        //Stmt->stmt->bind_param_done= 1;
        Stmt->RebindParams= FALSE;
      }

      ret= MADB_DoExecute(Stmt);

      ++Stmt->ArrayOffset;
      /* We need to unset InternalLength, i.e. reset dae length counters for next stmt.
  However that length is not used anywhere, and is not clear what is it needed for */
      ResetInternalLength(Stmt, ParamOffset);

      if (!SQL_SUCCEEDED(ret))
      {
        ++ErrorCount;
        if (Stmt->Ipd->Header.ArrayStatusPtr)
        {
          Stmt->Ipd->Header.ArrayStatusPtr[j - Start]= 
            (j == Start + Stmt->Apd->Header.ArraySize - 1) ? SQL_PARAM_ERROR : SQL_PARAM_DIAG_UNAVAILABLE;
        }
        if (j == Start + Stmt->Apd->Header.ArraySize - 1)
        {
          goto end;
        }
      }
      else
      {
        /* We had result from type conversions, thus here we put row as 1(!=0, i.e. not first) */
        CALC_ALL_ROWS_RC(IntegralRc, ret, 1);
        if (Stmt->Ipd->Header.ArrayStatusPtr)
        {
          Stmt->Ipd->Header.ArrayStatusPtr[j - Start]= SQL_PARAM_SUCCESS;
        }
      }
    }     /* End of for() thru paramsets(parameters array) */
  }       /* End of if (bulk/not bulk) execution */
  
  /* All rows processed, so we can unset ArrayOffset */
  Stmt->ArrayOffset= 0;

  if (Stmt->rs)
  {
    /*************************** mysql_stmt_store_result ******************************/
    /*If we did OUT params already, we should not store */
    if (Stmt->State == MADB_SS_EXECUTED)
    {
     /* This should go somewhere in the catch of exception on rs object creation
      if (DefaultResult)
      {
        mysql_free_result(DefaultResult);
      }
      return MADB_SetNativeError(&Stmt->Error, SQL_HANDLE_STMT, Stmt->stmt.get());*/
    }
    
    /* I don't think we can reliably establish the fact that we do not need to re-fetch the metadata, thus we are re-fetching always
       The fact that we have resultset has been established above in "if" condition(fields count is > 0) */
    FetchMetadata(Stmt);
    MADB_StmtResetResultStructures(Stmt);
    MADB_DescSetIrdMetadata(Stmt, Stmt->metadata->getFields(), Stmt->metadata->getColumnCount());

    Stmt->AffectedRows= -1;
  }
end:
  UNLOCK_MARIADB(Stmt->Connection);
  Stmt->LastRowFetched= 0;

  if (DefaultResult)
    mysql_free_result(DefaultResult);

  if (ErrorCount)
  {
    if (ErrorCount < Stmt->Apd->Header.ArraySize)
      IntegralRc= SQL_SUCCESS_WITH_INFO;
    else
      IntegralRc= SQL_ERROR;
  }

  if (IntegralRc == SQL_NEED_DATA && !Stmt->stmt->isServerSide()) {
    
    try {
      ServerSidePreparedStatement* ssps = new ServerSidePreparedStatement(Stmt->Connection->mariadb, STMT_STRING(Stmt),
        Stmt->Options.CursorType);
      Stmt->stmt.reset(ssps);
    }
    catch (int32_t) {
      // going further with csps
    }
  }

  return IntegralRc;
}
/* }}} */

/* {{{ MADB_StmtBindCol */
SQLRETURN MADB_StmtBindCol(MADB_Stmt *Stmt, SQLUSMALLINT ColumnNumber, SQLSMALLINT TargetType,
    SQLPOINTER TargetValuePtr, SQLLEN BufferLength, SQLLEN *StrLen_or_Ind)
{
  MADB_Desc *Ard= Stmt->Ard;
  MADB_DescRecord *Record;

  if ((ColumnNumber < 1 && Stmt->Options.UseBookmarks == SQL_UB_OFF) || 
       Stmt->rs && STMT_WAS_PREPARED(Stmt) &&
       ColumnNumber > Stmt->metadata->getColumnCount())
  {
    MADB_SetError(&Stmt->Error, MADB_ERR_07009, NULL, 0);
    return SQL_ERROR;
  }

  /* Bookmark */
  if (ColumnNumber == 0)
  {
    if (TargetType == SQL_C_BOOKMARK || TargetType == SQL_C_VARBOOKMARK)
    {
      Stmt->Options.BookmarkPtr=     TargetValuePtr;
      Stmt->Options.BookmarkLength = BufferLength;
      Stmt->Options.BookmarkType=    TargetType;
      return SQL_SUCCESS;
    }
    MADB_SetError(&Stmt->Error, MADB_ERR_07006, NULL, 0);
    return Stmt->Error.ReturnValue;
  }

  if (!(Record= MADB_DescGetInternalRecord(Ard, ColumnNumber - 1, MADB_DESC_WRITE)))
  {
    MADB_CopyError(&Stmt->Error, &Ard->Error);
    return Stmt->Error.ReturnValue;
  }

  /* check if we need to unbind and delete a record */
  if (!TargetValuePtr && !StrLen_or_Ind)
  {
    int i;
    Record->inUse= 0;
    /* Update counter */
    for (i= Ard->Records.elements; i > 0; i--)
    {
      MADB_DescRecord *Rec= MADB_DescGetInternalRecord(Ard, i-1, MADB_DESC_READ);
      if (Rec && Rec->inUse)
      {
        Ard->Header.Count= i;
        return SQL_SUCCESS;
      }
    }
    Ard->Header.Count= 0;
    return SQL_SUCCESS;
  }

  if (!SQL_SUCCEEDED(MADB_DescSetField(Ard, ColumnNumber, SQL_DESC_TYPE, (SQLPOINTER)(SQLLEN)TargetType, SQL_IS_SMALLINT, 0)) ||
      !SQL_SUCCEEDED(MADB_DescSetField(Ard, ColumnNumber, SQL_DESC_OCTET_LENGTH_PTR, (SQLPOINTER)StrLen_or_Ind, SQL_IS_POINTER, 0)) ||
      !SQL_SUCCEEDED(MADB_DescSetField(Ard, ColumnNumber, SQL_DESC_INDICATOR_PTR, (SQLPOINTER)StrLen_or_Ind, SQL_IS_POINTER, 0)) ||
      !SQL_SUCCEEDED(MADB_DescSetField(Ard, ColumnNumber, SQL_DESC_OCTET_LENGTH, (SQLPOINTER)MADB_GetTypeLength(TargetType, BufferLength), SQL_IS_INTEGER, 0)) ||
      !SQL_SUCCEEDED(MADB_DescSetField(Ard, ColumnNumber, SQL_DESC_DATA_PTR, TargetValuePtr, SQL_IS_POINTER, 0)))
  {
    MADB_CopyError(&Stmt->Error, &Ard->Error);
    return Stmt->Error.ReturnValue;
  }
   
  return SQL_SUCCESS;
}
/* }}} */

/* {{{ MADB_StmtBindParam */
SQLRETURN MADB_StmtBindParam(MADB_Stmt *Stmt,  SQLUSMALLINT ParameterNumber,
                             SQLSMALLINT InputOutputType, SQLSMALLINT ValueType,
                             SQLSMALLINT ParameterType, SQLULEN ColumnSize,
                             SQLSMALLINT DecimalDigits, SQLPOINTER ParameterValuePtr,
                             SQLLEN BufferLength, SQLLEN *StrLen_or_IndPtr)
{
   MADB_Desc *Apd= Stmt->Apd, 
             *Ipd= Stmt->Ipd;
   MADB_DescRecord *ApdRecord, *IpdRecord;
   SQLRETURN ret= SQL_SUCCESS;

   MADB_CLEAR_ERROR(&Stmt->Error);
   if (!(ApdRecord= MADB_DescGetInternalRecord(Apd, ParameterNumber - 1, MADB_DESC_WRITE)))
   {
     MADB_CopyError(&Stmt->Error, &Apd->Error);
     return Stmt->Error.ReturnValue;
   }
   if (!(IpdRecord= MADB_DescGetInternalRecord(Ipd, ParameterNumber - 1, MADB_DESC_WRITE)))
   {
     MADB_CopyError(&Stmt->Error, &Ipd->Error);
     return Stmt->Error.ReturnValue;
   }

   /* Map to the correspoinding type */
   if (ValueType == SQL_C_DEFAULT)
   {
     ValueType= MADB_GetDefaultType(ParameterType);
   }
   
   if (!(SQL_SUCCEEDED(MADB_DescSetField(Apd, ParameterNumber, SQL_DESC_CONCISE_TYPE, (SQLPOINTER)(SQLLEN)ValueType, SQL_IS_SMALLINT, 0))) ||
       !(SQL_SUCCEEDED(MADB_DescSetField(Apd, ParameterNumber, SQL_DESC_OCTET_LENGTH_PTR, (SQLPOINTER)StrLen_or_IndPtr, SQL_IS_POINTER, 0))) ||
       !(SQL_SUCCEEDED(MADB_DescSetField(Apd, ParameterNumber, SQL_DESC_OCTET_LENGTH, (SQLPOINTER)MADB_GetTypeLength(ValueType, BufferLength), SQL_IS_INTEGER, 0))) ||
       !(SQL_SUCCEEDED(MADB_DescSetField(Apd, ParameterNumber, SQL_DESC_INDICATOR_PTR, (SQLPOINTER)StrLen_or_IndPtr, SQL_IS_POINTER, 0))) ||
       !(SQL_SUCCEEDED(MADB_DescSetField(Apd, ParameterNumber, SQL_DESC_DATA_PTR, ParameterValuePtr, SQL_IS_POINTER, 0))))
   {
     MADB_CopyError(&Stmt->Error, &Apd->Error);
     return Stmt->Error.ReturnValue;
   }

   if (!(SQL_SUCCEEDED(MADB_DescSetField(Ipd, ParameterNumber, SQL_DESC_CONCISE_TYPE, (SQLPOINTER)(SQLLEN)ParameterType, SQL_IS_SMALLINT, 0))) ||
       !(SQL_SUCCEEDED(MADB_DescSetField(Ipd, ParameterNumber, SQL_DESC_PARAMETER_TYPE, (SQLPOINTER)(SQLLEN)InputOutputType, SQL_IS_SMALLINT, 0))))
   {
     MADB_CopyError(&Stmt->Error, &Ipd->Error);
     return Stmt->Error.ReturnValue;
   }

   switch(ParameterType) {
   case SQL_BINARY:
   case SQL_VARBINARY:
   case SQL_LONGVARBINARY:
   case SQL_CHAR:
   case SQL_VARCHAR:
   case SQL_LONGVARCHAR:
   case SQL_WCHAR:
   case SQL_WLONGVARCHAR:
   case SQL_WVARCHAR:
     ret= MADB_DescSetField(Ipd, ParameterNumber, SQL_DESC_LENGTH, (SQLPOINTER)ColumnSize, SQL_IS_INTEGER, 0);
     break;
   case SQL_FLOAT:
   case SQL_REAL:
   case SQL_DOUBLE:
     ret= MADB_DescSetField(Ipd, ParameterNumber, SQL_DESC_PRECISION, (SQLPOINTER)ColumnSize, SQL_IS_INTEGER, 0);
     break;
   case SQL_DECIMAL:
   case SQL_NUMERIC:
     ret= MADB_DescSetField(Ipd, ParameterNumber, SQL_DESC_PRECISION, (SQLPOINTER)ColumnSize, SQL_IS_SMALLINT, 0);
     if (SQL_SUCCEEDED(ret))
       ret= MADB_DescSetField(Ipd, ParameterNumber, SQL_DESC_SCALE, (SQLPOINTER)(SQLLEN)DecimalDigits, SQL_IS_SMALLINT, 0);
     break;
   case SQL_INTERVAL_MINUTE_TO_SECOND:
   case SQL_INTERVAL_HOUR_TO_SECOND:
   case SQL_INTERVAL_DAY_TO_SECOND:
   case SQL_INTERVAL_SECOND:
   case SQL_TYPE_TIMESTAMP:
   case SQL_TYPE_TIME:
     ret= MADB_DescSetField(Ipd, ParameterNumber, SQL_DESC_PRECISION, (SQLPOINTER)(SQLLEN)DecimalDigits, SQL_IS_SMALLINT, 0);
     break;
   }

   if(!SQL_SUCCEEDED(ret))
     MADB_CopyError(&Stmt->Error, &Ipd->Error);
   Stmt->RebindParams= TRUE;
   
   return ret;
 }
 /* }}} */

void MADB_InitStatusPtr(SQLUSMALLINT *Ptr, SQLULEN Size, SQLSMALLINT InitialValue)
{
  SQLULEN i;

  for (i=0; i < Size; i++)
    Ptr[i]= InitialValue;
}

/* Not used for now, but leaving it so far here - it may be useful */
/* BOOL MADB_NumericBufferType(SQLSMALLINT BufferType)
{
  switch (BufferType)
  {
  case SQL_C_TINYINT:
  case SQL_C_UTINYINT:
  case SQL_C_STINYINT:
  case SQL_C_SHORT:
  case SQL_C_SSHORT:
  case SQL_C_USHORT:
  case SQL_C_FLOAT:
  case SQL_C_LONG:
  case SQL_C_ULONG:
  case SQL_C_SLONG:
  case SQL_C_DOUBLE:
    return TRUE;
  default:
    return FALSE;
  }
}*/

/* {{{ MADB_BinaryFieldType */
BOOL MADB_BinaryFieldType(SQLSMALLINT FieldType)
{
  return FieldType == SQL_BINARY || FieldType == SQL_BIT;
}
/* }}} */

/* {{{ MADB_PrepareBind
       Filling bind structures in */
SQLRETURN MADB_PrepareBind(MADB_Stmt *Stmt, int RowNumber)
{
  MADB_DescRecord *IrdRec, *ArdRec;
  int             i;
  void            *DataPtr= NULL;

  for (i= 0; i < MADB_STMT_COLUMN_COUNT(Stmt); ++i)
  {
    SQLSMALLINT ConciseType;
    ArdRec= MADB_DescGetInternalRecord(Stmt->Ard, i, MADB_DESC_READ);

    /* We can't use application's buffer directly, as it has/can have different size, than C/C needs */
    Stmt->result[i].length = &Stmt->result[i].length_value;
    Stmt->result[i].is_null = &Stmt->result[i].is_null_value;

    if (ArdRec == NULL || !ArdRec->inUse)
    {      
      Stmt->result[i].flags|= MADB_BIND_DUMMY;
      continue;
    }

    DataPtr= (SQLLEN *)GetBindOffset(Stmt->Ard, ArdRec, ArdRec->DataPtr, RowNumber, ArdRec->OctetLength);

    MADB_FREE(ArdRec->InternalBuffer);
    if (!DataPtr)
    {
      Stmt->result[i].flags|= MADB_BIND_DUMMY;
      continue;
    }
    else
    {
      Stmt->result[i].flags&= ~MADB_BIND_DUMMY;
    }

    IrdRec= MADB_DescGetInternalRecord(Stmt->Ird, i, MADB_DESC_READ);
    /* assert(IrdRec != NULL) */

    ConciseType= ArdRec->ConciseType;
    if (ConciseType == SQL_C_DEFAULT)
    {
      ConciseType= IrdRec->ConciseType;
    }
    switch(ConciseType) {
    case SQL_C_WCHAR:
      /* In worst case for 2 bytes of UTF16 in result, we need 3 bytes of utf8.
          For ASCII  we need 2 times less(for 2 bytes of UTF16 - 1 byte UTF8,
          in other cases we need same 2 of 4 bytes. */
      ArdRec->InternalBuffer=        (char *)MADB_CALLOC((size_t)((ArdRec->OctetLength)*1.5));
      Stmt->result[i].buffer=        ArdRec->InternalBuffer;
      Stmt->result[i].buffer_length= (unsigned long)(ArdRec->OctetLength*1.5);
      Stmt->result[i].buffer_type=   MYSQL_TYPE_STRING;
      break;
    case SQL_C_CHAR:
      Stmt->result[i].buffer=        DataPtr;
      Stmt->result[i].buffer_length= (unsigned long)ArdRec->OctetLength;
      Stmt->result[i].buffer_type=   MYSQL_TYPE_STRING;
      break;
    case SQL_C_NUMERIC:
      MADB_FREE(ArdRec->InternalBuffer);
      Stmt->result[i].buffer_length= MADB_DEFAULT_PRECISION + 1/*-*/ + 1/*.*/;
      ArdRec->InternalBuffer=       (char *)MADB_CALLOC(Stmt->result[i].buffer_length);
      Stmt->result[i].buffer=        ArdRec->InternalBuffer;
      
      Stmt->result[i].buffer_type=   MYSQL_TYPE_STRING;
      break;
    case SQL_TYPE_TIMESTAMP:
    case SQL_TYPE_DATE:
    case SQL_TYPE_TIME:
    case SQL_C_TIMESTAMP:
    case SQL_C_TIME:
    case SQL_C_DATE:
      MADB_FREE(ArdRec->InternalBuffer);
      if (IrdRec->ConciseType == SQL_CHAR || IrdRec->ConciseType == SQL_VARCHAR)
      {
        const MYSQL_FIELD *field= Stmt->metadata->getField(i);
        Stmt->result[i].buffer_length= (field->max_length != 0 ?
          field->max_length : field->length) + 1;
        ArdRec->InternalBuffer= (char *)MADB_CALLOC(Stmt->result[i].buffer_length);
        if (ArdRec->InternalBuffer == NULL)
        {
          return MADB_SetError(&Stmt->Error, MADB_ERR_HY001, NULL, 0);
        }
        Stmt->result[i].buffer=        ArdRec->InternalBuffer;
        Stmt->result[i].buffer_type=   MYSQL_TYPE_STRING;
      }
      else
      {
        ArdRec->InternalBuffer=       (char *)MADB_CALLOC(sizeof(MYSQL_TIME));
        Stmt->result[i].buffer=        ArdRec->InternalBuffer;
        Stmt->result[i].buffer_length= sizeof(MYSQL_TIME);
        Stmt->result[i].buffer_type=   MYSQL_TYPE_TIMESTAMP;
      }
      break;
    case SQL_C_INTERVAL_HOUR_TO_MINUTE:
    case SQL_C_INTERVAL_HOUR_TO_SECOND:
      {
        const MYSQL_FIELD *Field= Stmt->metadata->getField(i);
        MADB_FREE(ArdRec->InternalBuffer);
        if (IrdRec->ConciseType == SQL_CHAR || IrdRec->ConciseType == SQL_VARCHAR)
        {
          Stmt->result[i].buffer_length = (Field->max_length != 0 ?
            Field->max_length : Field->length) + 1;
          ArdRec->InternalBuffer= (char *)MADB_CALLOC(Stmt->result[i].buffer_length);
          if (ArdRec->InternalBuffer == NULL)
          {
            return MADB_SetError(&Stmt->Error, MADB_ERR_HY001, NULL, 0);
          }
          Stmt->result[i].buffer=        ArdRec->InternalBuffer;
          Stmt->result[i].buffer_type=   MYSQL_TYPE_STRING;
        }
        else
        {
          ArdRec->InternalBuffer=       (char *)MADB_CALLOC(sizeof(MYSQL_TIME));
          Stmt->result[i].buffer=        ArdRec->InternalBuffer;
          Stmt->result[i].buffer_length= sizeof(MYSQL_TIME);
          Stmt->result[i].buffer_type=   Field && Field->type == MYSQL_TYPE_TIME ? MYSQL_TYPE_TIME : MYSQL_TYPE_TIMESTAMP;
        }
      }
      break;
    case SQL_C_UTINYINT:
    case SQL_C_USHORT:
    case SQL_C_ULONG:
      Stmt->result[i].is_unsigned= '\1';
    case SQL_C_TINYINT:
    case SQL_C_STINYINT:
    case SQL_C_SHORT:
    case SQL_C_SSHORT:
    case SQL_C_FLOAT:
    case SQL_C_LONG:
    case SQL_C_SLONG:
    case SQL_C_DOUBLE:
      if (MADB_BinaryFieldType(IrdRec->ConciseType))
      {
        /* To keep things simple - we will use internal buffer of the column size, and later(in the MADB_FixFetchedValues) will copy (correct part of)
           it to the application's buffer taking care of endianness. Perhaps it'd be better just not to support this type of conversion */
        MADB_FREE(ArdRec->InternalBuffer);
        ArdRec->InternalBuffer=        (char *)MADB_CALLOC(IrdRec->OctetLength);
        Stmt->result[i].buffer=        ArdRec->InternalBuffer;
        Stmt->result[i].buffer_length= (unsigned long)IrdRec->OctetLength;
        Stmt->result[i].buffer_type=   MYSQL_TYPE_BLOB;
        break;
      }
      /* else {we are falling through below} */
    default:
      if (!MADB_CheckODBCType(ArdRec->ConciseType))
      {
        return MADB_SetError(&Stmt->Error, MADB_ERR_07006, NULL, 0);
      }
      Stmt->result[i].buffer_length= (unsigned long)ArdRec->OctetLength;
      Stmt->result[i].buffer=        DataPtr;
      Stmt->result[i].buffer_type=   MADB_GetMaDBTypeAndLength(ConciseType,
                                                            &Stmt->result[i].is_unsigned,
                                                            &Stmt->result[i].buffer_length);
      break;
    }
  }

  return SQL_SUCCESS;
}
/* }}} */

/* {{{ LittleEndian */
char LittleEndian()
{
  int   x= 1;
  char *c= (char*)&x;

  return *c;
}
/* }}} */

/* {{{ SwitchEndianness */
void SwitchEndianness(char *Src, SQLLEN SrcBytes, char *Dst, SQLLEN DstBytes)
{
  /* SrcBytes can only be less or equal DstBytes */
  while (SrcBytes--)
  {
    *Dst++= *(Src + SrcBytes);
  }
}
/* }}} */

#define CALC_ALL_FLDS_RC(_agg_rc, _field_rc) if (_field_rc != SQL_SUCCESS && _agg_rc != SQL_ERROR) _agg_rc= _field_rc 

/* {{{ MADB_FixFetchedValues 
       Converting and/or fixing fetched values if needed */
SQLRETURN MADB_FixFetchedValues(MADB_Stmt *Stmt, int RowNumber, int64_t SaveCursor)
{
  MADB_DescRecord *IrdRec, *ArdRec;
  int             i;
  SQLLEN          *IndicatorPtr= NULL, *LengthPtr= NULL, Dummy= 0;
  void            *DataPtr=      NULL;
  SQLRETURN       rc= SQL_SUCCESS, FieldRc;

  for (i= 0; i < MADB_STMT_COLUMN_COUNT(Stmt); ++i)
  {
    if ((ArdRec= MADB_DescGetInternalRecord(Stmt->Ard, i, MADB_DESC_READ)) && ArdRec->inUse)
    {
      /* set indicator and dataptr */
      LengthPtr=    (SQLLEN *)GetBindOffset(Stmt->Ard, ArdRec, ArdRec->OctetLengthPtr, RowNumber, sizeof(SQLLEN));
      IndicatorPtr= (SQLLEN *)GetBindOffset(Stmt->Ard, ArdRec, ArdRec->IndicatorPtr,   RowNumber, sizeof(SQLLEN));
      DataPtr=      (SQLLEN *)GetBindOffset(Stmt->Ard, ArdRec, ArdRec->DataPtr,        RowNumber, ArdRec->OctetLength);

      if (LengthPtr == NULL)
      {
        LengthPtr= &Dummy;
      }
      /* clear IndicatorPtr */
      if (IndicatorPtr != NULL && IndicatorPtr != LengthPtr && *IndicatorPtr < 0)
      {
        *IndicatorPtr= 0;
      }

      IrdRec= MADB_DescGetInternalRecord(Stmt->Ird, i, MADB_DESC_READ);
      /* assert(IrdRec != NULL) */

      if (*Stmt->result[i].is_null)
      {
        if (IndicatorPtr)
        {
          *IndicatorPtr= SQL_NULL_DATA;
        }
        else
        {
          if (SaveCursor > 0)
          {
            Stmt->rs->absolute(SaveCursor);
          }
          rc= MADB_SetError(&Stmt->Error, MADB_ERR_22002, NULL, 0);
          continue;
        }
      }
      else
      {
        switch (ArdRec->ConciseType)
        {
        case SQL_C_BIT:
        {
          char *p= (char *)Stmt->result[i].buffer;
          if (p)
          {
            *p= test(*p != '\0');
          }
        }
        break;
        case SQL_C_TYPE_TIMESTAMP:
        case SQL_C_TYPE_DATE:
        case SQL_C_TYPE_TIME:
        case SQL_C_TIMESTAMP:
        case SQL_C_TIME:
        case SQL_C_DATE:
          {
            MYSQL_TIME tm, *Intermidiate;

            if (IrdRec->ConciseType == SQL_CHAR || IrdRec->ConciseType == SQL_VARCHAR)
            {
              BOOL isTime;

              FieldRc= MADB_Str2Ts(ArdRec->InternalBuffer, *Stmt->result[i].length, &tm, FALSE, &Stmt->Error, &isTime);
              if (SQL_SUCCEEDED(FieldRc))
              {
                Intermidiate= &tm;
              }
              else
              {
                CALC_ALL_FLDS_RC(rc, FieldRc);
                break;
              }
            }
            else
            {
              Intermidiate= (MYSQL_TIME *)ArdRec->InternalBuffer;
            }

            FieldRc= MADB_CopyMadbTimestamp(Stmt, Intermidiate, DataPtr, LengthPtr, IndicatorPtr, ArdRec->Type, IrdRec->ConciseType);
            CALC_ALL_FLDS_RC(rc, FieldRc);
          }
          break;
        case SQL_C_INTERVAL_HOUR_TO_MINUTE:
        case SQL_C_INTERVAL_HOUR_TO_SECOND:
        {
          MYSQL_TIME          *tm= (MYSQL_TIME*)ArdRec->InternalBuffer, ForConversion;
          SQL_INTERVAL_STRUCT *ts= (SQL_INTERVAL_STRUCT *)DataPtr;

          if (IrdRec->ConciseType == SQL_CHAR || IrdRec->ConciseType == SQL_VARCHAR)
          {
            BOOL isTime;

            FieldRc= MADB_Str2Ts(ArdRec->InternalBuffer, *Stmt->result[i].length, &ForConversion, FALSE, &Stmt->Error, &isTime);
            if (SQL_SUCCEEDED(FieldRc))
            {
              tm= &ForConversion;
            }
            else
            {
              CALC_ALL_FLDS_RC(rc, FieldRc);
              break;
            }
          }

          /* If we have ts == NULL we (may) have tm also NULL, since we didn't really bind this column */
          if (ts != NULL)
          {
            if (tm->hour > 99999)
            {
              FieldRc= MADB_SetError(&Stmt->Error, MADB_ERR_22015, NULL, 0);
              CALC_ALL_FLDS_RC(rc, FieldRc);
              break;
            }

            ts->intval.day_second.hour= tm->hour;
            ts->intval.day_second.minute= tm->minute;
            ts->interval_sign= tm->neg ? SQL_TRUE : SQL_FALSE;

            if (ArdRec->Type == SQL_C_INTERVAL_HOUR_TO_MINUTE)
            {
              ts->intval.day_second.second= 0;
              ts->interval_type= /*SQLINTERVAL::*/SQL_IS_HOUR_TO_MINUTE;
              if (tm->second)
              {
                FieldRc= MADB_SetError(&Stmt->Error, MADB_ERR_01S07, NULL, 0);
                CALC_ALL_FLDS_RC(rc, FieldRc);
                break;
              }
            }
            else
            {
              ts->interval_type= /*SQLINTERVAL::*/SQL_IS_HOUR_TO_SECOND;
              ts->intval.day_second.second= tm->second;
            }
          }
          
          *LengthPtr= sizeof(SQL_INTERVAL_STRUCT);
        }
        break;
        case SQL_C_NUMERIC:
        {
          int LocalRc= 0;
          MADB_CLEAR_ERROR(&Stmt->Error);
          if (DataPtr != NULL && Stmt->result[i].buffer_length < *Stmt->result[i].length)
          {
            MADB_SetError(&Stmt->Error, MADB_ERR_22003, NULL, 0);
            ArdRec->InternalBuffer[Stmt->result[i].buffer_length - 1]= 0;
            return Stmt->Error.ReturnValue;
          }

          if ((LocalRc= MADB_CharToSQLNumeric(ArdRec->InternalBuffer, Stmt->Ard, ArdRec, NULL, RowNumber)))
          {
            FieldRc= MADB_SetError(&Stmt->Error, LocalRc, NULL, 0);
            CALC_ALL_FLDS_RC(rc, FieldRc);
          }
          /* TODO: why is it here individually for Numeric type?! */
          if (Stmt->Ard->Header.ArrayStatusPtr)
          {
            Stmt->Ard->Header.ArrayStatusPtr[RowNumber]= Stmt->Error.ReturnValue;
          }
          *LengthPtr= sizeof(SQL_NUMERIC_STRUCT);
        }
        break;
        case SQL_C_WCHAR:
        {
          SQLLEN CharLen= MADB_SetString(&Stmt->Connection->Charset, DataPtr, ArdRec->OctetLength/sizeof(SQLWCHAR), (char *)Stmt->result[i].buffer,
            *Stmt->result[i].length, &Stmt->Error);

          /* If returned len is 0 while source len is not - taking it as error occurred */
          if ((CharLen == 0 ||
            (SQLULEN)CharLen > (ArdRec->OctetLength / sizeof(SQLWCHAR))) && *Stmt->result[i].length != 0 && Stmt->result[i].buffer != NULL &&
            *(char*)Stmt->result[i].buffer != '\0' && Stmt->Error.ReturnValue != SQL_SUCCESS)
          {
            CALC_ALL_FLDS_RC(rc, Stmt->Error.ReturnValue);
          }
          /* If application didn't give data buffer and only want to know the length of data to fetch */
          if (CharLen == 0 && *Stmt->result[i].length != 0 && Stmt->result[i].buffer == NULL)
          {
            CharLen= *Stmt->result[i].length;
          }
          /* Not quite right */
          *LengthPtr = CharLen * sizeof(SQLWCHAR);
        }
        break;

        case SQL_C_TINYINT:
        case SQL_C_UTINYINT:
        case SQL_C_STINYINT:
        case SQL_C_SHORT:
        case SQL_C_SSHORT:
        case SQL_C_USHORT:
        case SQL_C_FLOAT:
        case SQL_C_LONG:
        case SQL_C_ULONG:
        case SQL_C_SLONG:
        case SQL_C_DOUBLE:
          if (MADB_BinaryFieldType(IrdRec->ConciseType))
          {
            if (DataPtr != NULL)
            {
              if (Stmt->result[i].buffer_length >= (unsigned long)ArdRec->OctetLength)
              {
                if (LittleEndian())
                {
                  /* We currently got the bigendian number. If we or littleendian machine, we need to switch bytes */
                  SwitchEndianness((char*)Stmt->result[i].buffer + Stmt->result[i].buffer_length - ArdRec->OctetLength,
                    ArdRec->OctetLength,
                    (char*)DataPtr,
                    ArdRec->OctetLength);
                }
                else
                {
                  memcpy(DataPtr, (void*)((char*)Stmt->result[i].buffer + Stmt->result[i].buffer_length - ArdRec->OctetLength), ArdRec->OctetLength);
                }
              }
              else
              {
                /* We won't write to the whole memory pointed by DataPtr, thus to need to zerofill prior to that */
                memset(DataPtr, 0, ArdRec->OctetLength);
                if (LittleEndian())
                {
                  SwitchEndianness((char*)Stmt->result[i].buffer,
                    Stmt->result[i].buffer_length,
                    (char*)DataPtr,
                    ArdRec->OctetLength);
                }
                else
                {
                  memcpy((void*)((char*)DataPtr + ArdRec->OctetLength - Stmt->result[i].buffer_length),
                    Stmt->result[i].buffer, Stmt->result[i].buffer_length);
                }
              }
              *LengthPtr= *Stmt->result[i].length;
            }
            break;
          }
          /* else {we are falling through below} */
        default:
          if (DataPtr != NULL)
          {
            if (Stmt->Ard->Header.ArraySize > 1)
            {
              if (Stmt->Ard->Header.BindType)
              {
                Stmt->result[i].buffer= (char *)Stmt->result[i].buffer + Stmt->Ard->Header.BindType;
              }
              else
              {
                Stmt->result[i].buffer = (char *)ArdRec->DataPtr + (RowNumber + 1) * ArdRec->OctetLength;
              }
            }
            *LengthPtr= *Stmt->result[i].length;
          }
          break;
        }
      }
    }
  }

  return rc;
}
/* }}} */
#undef CALC_ALL_FLDS_RC


SQLUSMALLINT MADB_MapToRowStatus(SQLRETURN rc)
{
  switch (rc)
  {
  case SQL_SUCCESS_WITH_INFO: return SQL_ROW_SUCCESS_WITH_INFO;
  case SQL_ERROR:             return SQL_ROW_ERROR;
  /* Assuming is that status array pre-filled with SQL_ROW_NOROW,
     and it never needs to be mapped to */
  }

  return SQL_ROW_SUCCESS;
}


void ResetDescIntBuffers(MADB_Desc *Desc)
{
  MADB_DescRecord *Rec;
  SQLSMALLINT i;

  for (i= 0; i < Desc->Header.Count; ++i)
  {
    Rec= MADB_DescGetInternalRecord(Desc, i, MADB_DESC_READ);
    if (Rec)
    {
      MADB_FREE(Rec->InternalBuffer);
    }
  }
}

/* Processes truncation errors occurred while row fetch */
SQLRETURN MADB_ProcessTruncation(MADB_Stmt *Stmt)
{
  /* We will not report truncation if a dummy buffer was bound */
  int     col;
  for (col = 0; col < MADB_STMT_COLUMN_COUNT(Stmt); ++col)
  {
    if (Stmt->result[col].error && *Stmt->result[col].error > 0 &&
      !(Stmt->result[col].flags & MADB_BIND_DUMMY))
    {
      MADB_DescRecord* ArdRec = MADB_DescGetInternalRecord(Stmt->Ard, col, MADB_DESC_READ),
        * IrdRec = MADB_DescGetInternalRecord(Stmt->Ird, col, MADB_DESC_READ);
      /* If (numeric) field value and buffer are of the same size - ignoring truncation.
      In some cases specs are not clear enough if certain column signed or not(think of catalog functions for example), and
      some apps bind signed buffer where we return unsigdned value. And in general - if application want to fetch unsigned as
      signed, or vice versa, why we should prevent that. */
      if (ArdRec->OctetLength == IrdRec->OctetLength
        && MADB_IsIntType(IrdRec->ConciseType) && (ArdRec->ConciseType == SQL_C_DEFAULT || MADB_IsIntType(ArdRec->ConciseType)))
      {
        continue;
      }
      /* For numeric types we return either 22003 or 01S07, 01004 for the rest.
         if ird type is not fractional - we return 22003. But as a matter of fact, it's possible that we have 22003 if converting
         from fractional types */
      return MADB_SetError(&Stmt->Error, ArdRec != NULL && MADB_IsNumericType(ArdRec->ConciseType) ?
        (MADB_IsIntType(IrdRec->ConciseType) ? MADB_ERR_22003 : MADB_ERR_01S07) : MADB_ERR_01004, NULL, 0);
      /* One found such column is enough */
    }
  }
  return SQL_SUCCESS;
}

/* {{{ MADB_StmtFetch */
SQLRETURN MADB_StmtFetch(MADB_Stmt *Stmt)
{
  unsigned int     RowNum, j;
  SQLULEN          Rows2Fetch=  Stmt->Ard->Header.ArraySize, Processed, *ProcessedPtr= &Processed;
  int64_t          SaveCursor= -1;
  SQLRETURN        Result= SQL_SUCCESS, RowResult;
  bool             Streaming= false; /* Also means the lock has been obtained */

  MADB_CLEAR_ERROR(&Stmt->Error);

  if (!(MADB_STMT_COLUMN_COUNT(Stmt) > 0))
  {
    return MADB_SetError(&Stmt->Error, MADB_ERR_24000, NULL, 0);
  }

  if ((Stmt->Options.UseBookmarks == SQL_UB_VARIABLE && Stmt->Options.BookmarkType == SQL_C_BOOKMARK) ||
      (Stmt->Options.UseBookmarks != SQL_UB_VARIABLE && Stmt->Options.BookmarkType == SQL_C_VARBOOKMARK))
  {
    MADB_SetError(&Stmt->Error, MADB_ERR_07006, NULL, 0);
    return Stmt->Error.ReturnValue;
  }

  /* We don't have much to do if ArraySize == 0 */
  if (Stmt->Ard->Header.ArraySize == 0)
  {
    return SQL_SUCCESS;
  }

  Stmt->LastRowFetched= 0;
  Rows2Fetch= MADB_RowsToFetch(&Stmt->Cursor, Stmt->Ard->Header.ArraySize,
    MADB_STMT_SHOULD_STREAM(Stmt) ? (unsigned long long)-1 : Stmt->rs->rowsCount());

  if (Stmt->result == NULL)
  {
    if (!(Stmt->result= (MYSQL_BIND *)MADB_CALLOC(sizeof(MYSQL_BIND) * Stmt->metadata->getColumnCount())))
    {
      MADB_SetError(&Stmt->Error, MADB_ERR_HY001, NULL, 0);
      return Stmt->Error.ReturnValue;
    }
    if (Rows2Fetch > 1)
    {
      // We need something to be bound after executing for MoveNext function
      Stmt->rs->bind(Stmt->result);
    }
  }

  if (Rows2Fetch == 0)
  {
    return SQL_NO_DATA;
  }

  if (Stmt->Ard->Header.ArrayStatusPtr)
  {
    MADB_InitStatusPtr(Stmt->Ard->Header.ArrayStatusPtr, Stmt->Ard->Header.ArraySize, SQL_NO_DATA);
  }

  if (Stmt->Ird->Header.RowsProcessedPtr)
  {
    ProcessedPtr= Stmt->Ird->Header.RowsProcessedPtr;
  }
  if (Stmt->Ird->Header.ArrayStatusPtr)
  {
    MADB_InitStatusPtr(Stmt->Ird->Header.ArrayStatusPtr, Stmt->Ard->Header.ArraySize, SQL_ROW_NOROW);
  }

  *ProcessedPtr= 0;

  /* We need to return to 1st row in the rowset only if there are >1 rows in it. Otherwise we stay on it anyway */
  if (Rows2Fetch > 1 && Stmt->Options.CursorType != SQL_CURSOR_FORWARD_ONLY)
  {
    SaveCursor= Stmt->rs->getRow();
    /* Skipping current row for for reading now, it will be read when the Cursor is returned to it */
    MoveNext(Stmt, 1LL);
  }

  /* In case or streaming we need to guard fetch as well. It's actually can be that keeping mutex locked all the time the RS being fetched */
  /* But we don't want to lock here each time, even when streaming is not an option.
     What basically can happen if  we read current streamer not under the lock
     1) We read NULL or other Stmt, and do not lock. Even if this Stmt was a streamer, and other thread has changed that - means it has initiated caching of
        this Stmt data, and lock is not needed
     2) We read this Stmt is a streamer, while it's been changed by other thread - basically this is like 1), but due to race condition, it's not finished, and
        this thread thinks this Stmt is still the streamer. It tries to lock, and maybe will have to wait till its release. By the time of obtaining the lock,
        caching is finished, and we are safe to read our row(s). One unnecessary locking
     3) We read, we are not the streamer, and do not lock, and other thread changes that - up to releasing the handle. Uh... oh... so, threads share Stmt, other
        thread finishes reading, issue another query, that makes this a streamer, or this fetch was started even before other stream finished execution of the
        query. Hmm... Well, does not look possible/probable. If happens - syncing is on application. And third - don't share statement handle between threads :)
     Looks like reading Streamer before locking is safe. While we can have MADB_STMT_SHOULD_STREAM true after the result has been cached, and no lock is needed.
     4) TODO: Do we really need to LOCK if this Stmt is a streamer? Ok,it can save the day, if that is the last fetch for the RS, otherwise if other Stmt wants
     the lock, it will break the protocol after this row fetched and lock released. Letting along the fact, that it shouldn't even supposed to get to the point
     of lock requesting. Once we have caching of the rest of the streamed RS in place, maybe this syncing will be needed.
   */
    
  /*if (MADB_STMT_IS_STREAMING(Stmt))
  {
    LOCK_MARIADB(Stmt->Connection);*/
    /* We need this local flag while MADB_STMT_IS_STREAMING(Stmt) value may change due to item 2 from above, or even this function can change it, if fetch
       returns no */
    /*Streaming= TRUE;
  }*/

  for (j= 0; j < Rows2Fetch; ++j)
  {
    RowResult= SQL_SUCCESS;
    /* If we need to return the cursor to 1st row in the rowset, we start to read it from 2nd, and 1st row we read the last */
    if (SaveCursor != -1)
    {
      RowNum= j + 1;
      if (RowNum == Rows2Fetch)
      {
        RowNum= 0;
        Stmt->Cursor.Next= Stmt->rs->getRow();
        Stmt->rs->absolute(SaveCursor);
      }
    }
    else
    {
      RowNum= j;
    }
    /*************** Setting up BIND structures ********************/
    /* Basically, nothing should happen here, but if happens, then it will happen on each row.
    Thus it's ok to stop */
    RETURN_ERROR_OR_CONTINUE(MADB_PrepareBind(Stmt, RowNum));

    /************************ Bind! ********************************/  
    Stmt->rs->bind(Stmt->result);

    if (Stmt->Options.UseBookmarks && Stmt->Options.BookmarkPtr != NULL)
    {
      /* TODO: Bookmark can be not only "unsigned long*", but also "unsigned char*". Can be determined by examining Stmt->Options.BookmarkType */
      long *p= (long *)Stmt->Options.BookmarkPtr;
      p+= RowNum * Stmt->Options.BookmarkLength;
      *p= (long)Stmt->Cursor.Position;
    }
    /************************ Fetch! ********************************/
    try
    {
      /* Something, that we need to do even if fetch fails */
      *ProcessedPtr += 1;
      if (Stmt->Cursor.Position <= 0)
      {
        Stmt->Cursor.Position= 1;
      }
      if (!Stmt->rs->next()) {
        
        /* We have already incremented this counter, since there was no more rows, need to decrement */
        --*ProcessedPtr;
        /* Here we prefer to use not the local, but global flag */
        if (MADB_STMT_IS_STREAMING(Stmt))
        {
          if (!HasMoreResults(Stmt->Connection))
          {
            MADB_RESET_STREAMER(Stmt->Connection);
          }
        }
        /* SQL_NO_DATA should be only returned if first fetched row is already beyond end of the resultset */
        if (RowNum > 0)
        {
          continue;
        }
        /*if (Streaming != FALSE)
        {
          UNLOCK_MARIADB(Stmt->Connection);
        }*/
        return SQL_NO_DATA;
      }
      if (Stmt->rs->get())
      {
        RowResult= MADB_ProcessTruncation(Stmt);
      }
    }
    catch (int rc)
    {
      switch (rc) {
      case 1:
        RowResult= MADB_SetNativeError(&Stmt->Error, SQL_HANDLE_STMT, Stmt->stmt.get());
        /* If mysql_stmt_fetch returned error, there is no sense to continue */
        if (Stmt->Ird->Header.ArrayStatusPtr)
        {
          Stmt->Ird->Header.ArrayStatusPtr[RowNum]= MADB_MapToRowStatus(RowResult);
        }
        CALC_ALL_ROWS_RC(Result, RowResult, RowNum);
        /*if (Streaming != FALSE)
        {
          UNLOCK_MARIADB(Stmt->Connection);
        }*/
        return Result;

      case MYSQL_DATA_TRUNCATED:
      {
        RowResult= MADB_ProcessTruncation(Stmt);
        break;
      }
      }  /* End of switch on fetch result */
    }
    catch (SQLException& e) {
      RowResult= MADB_FromException(Stmt->Error, e);
    }
    catch (std::invalid_argument &ia) {
      RowResult= MADB_SetError(&Stmt->Error, MADB_ERR_22018, ia.what(), 0);
    }
    catch (std::out_of_range &oor) {
      RowResult= MADB_SetError(&Stmt->Error, MADB_ERR_22003, oor.what(), 0);
    } /* end of catch block */
    ++Stmt->LastRowFetched;
    ++Stmt->PositionedCursor;

    /*Conversion etc. At this point, after fetch we can have RowResult either SQL_SUCCESS or SQL_SUCCESS_WITH_INFO */
    switch (MADB_FixFetchedValues(Stmt, RowNum, SaveCursor))
    {
    case SQL_ERROR:
      RowResult= SQL_ERROR;
      break;
    case SQL_SUCCESS_WITH_INFO:
      RowResult= SQL_SUCCESS_WITH_INFO;
    /* And if result of conversions - success, just leaving that we had before */
    }

    CALC_ALL_ROWS_RC(Result, RowResult, RowNum);

    if (Stmt->Ird->Header.ArrayStatusPtr)
    {
      Stmt->Ird->Header.ArrayStatusPtr[RowNum]= MADB_MapToRowStatus(RowResult);
    }
  }
  /*if (Streaming != FALSE)
  {
    UNLOCK_MARIADB(Stmt->Connection);
  }*/
  memset(Stmt->CharOffset, 0, sizeof(long) * Stmt->metadata->getColumnCount());
  memset(Stmt->Lengths, 0, sizeof(long) * Stmt->metadata->getColumnCount());

  ResetDescIntBuffers(Stmt->Ird);

  return Result;
}
/* }}} */

#undef CALC_ALL_ROWS_RC

/* {{{ MADB_StmtGetAttr */ 
SQLRETURN MADB_StmtGetAttr(MADB_Stmt *Stmt, SQLINTEGER Attribute, SQLPOINTER ValuePtr, SQLINTEGER BufferLength,
                       SQLINTEGER *StringLengthPtr)
{
  SQLINTEGER StringLength;
  SQLRETURN ret= SQL_SUCCESS;

  if (!StringLengthPtr)
    StringLengthPtr= &StringLength;

  if (!Stmt)
    return SQL_INVALID_HANDLE;

  switch(Attribute) {
  case SQL_ATTR_APP_PARAM_DESC:
    *(SQLPOINTER *)ValuePtr= Stmt->Apd;
    *StringLengthPtr= sizeof(SQLPOINTER *);
    break;
  case SQL_ATTR_APP_ROW_DESC:
    *(SQLPOINTER *)ValuePtr= Stmt->Ard;
    *StringLengthPtr= sizeof(SQLPOINTER *);
    break;
  case SQL_ATTR_IMP_PARAM_DESC:
    *(SQLPOINTER *)ValuePtr= Stmt->Ipd;
    *StringLengthPtr= sizeof(SQLPOINTER *);
    break;
  case SQL_ATTR_IMP_ROW_DESC:
    *(SQLPOINTER *)ValuePtr= Stmt->Ird;
    *StringLengthPtr= sizeof(SQLPOINTER *);
    break;
  case SQL_ATTR_PARAM_BIND_OFFSET_PTR:
    *(SQLPOINTER *)ValuePtr= Stmt->Apd->Header.BindOffsetPtr;
    break;
  case SQL_ATTR_PARAM_BIND_TYPE:
    *(SQLULEN *)ValuePtr= Stmt->Apd->Header.BindType;
    break;
  case SQL_ATTR_PARAM_OPERATION_PTR:
    *(SQLPOINTER *)ValuePtr= (SQLPOINTER)Stmt->Apd->Header.ArrayStatusPtr;
    break;
  case SQL_ATTR_PARAM_STATUS_PTR:
    *(SQLPOINTER *)ValuePtr= (SQLPOINTER)Stmt->Ipd->Header.ArrayStatusPtr;
    break;
  case SQL_ATTR_PARAMS_PROCESSED_PTR:
    *(SQLPOINTER *)ValuePtr= (SQLPOINTER)(SQLULEN)Stmt->Ipd->Header.BindType;
    break;
  case SQL_ATTR_PARAMSET_SIZE:
    *(SQLULEN *)ValuePtr= Stmt->Apd->Header.ArraySize;
    break;
  case SQL_ATTR_ASYNC_ENABLE:
    *(SQLPOINTER *)ValuePtr= SQL_ASYNC_ENABLE_OFF;
    break;
  case SQL_ATTR_ROW_ARRAY_SIZE:
  case SQL_ROWSET_SIZE:
    *(SQLULEN *)ValuePtr= Stmt->Ard->Header.ArraySize;
    break;
  case SQL_ATTR_ROW_BIND_OFFSET_PTR:
    *(SQLPOINTER *)ValuePtr= (SQLPOINTER)Stmt->Ard->Header.BindOffsetPtr;
    break;
  case SQL_ATTR_ROW_BIND_TYPE:
    *(SQLULEN *)ValuePtr= Stmt->Ard->Header.BindType;
    break;
  case SQL_ATTR_ROW_OPERATION_PTR:
    *(SQLPOINTER *)ValuePtr= (SQLPOINTER)Stmt->Ard->Header.ArrayStatusPtr;
    break;
  case SQL_ATTR_ROW_STATUS_PTR:
    *(SQLPOINTER *)ValuePtr= (SQLPOINTER)Stmt->Ird->Header.ArrayStatusPtr;
    break;
  case SQL_ATTR_ROWS_FETCHED_PTR:
    *(SQLULEN **)ValuePtr= Stmt->Ird->Header.RowsProcessedPtr;
    break;
  case SQL_ATTR_USE_BOOKMARKS:
    *(SQLUINTEGER *)ValuePtr= Stmt->Options.UseBookmarks;
  case SQL_ATTR_SIMULATE_CURSOR:
    *(SQLULEN *)ValuePtr= Stmt->Options.SimulateCursor;
    break;
  case SQL_ATTR_CURSOR_SCROLLABLE:
    *(SQLULEN *)ValuePtr= Stmt->Options.CursorType;
    break;
  case SQL_ATTR_CURSOR_SENSITIVITY:
    *(SQLULEN *)ValuePtr= SQL_UNSPECIFIED;
    break;
  case SQL_ATTR_CURSOR_TYPE:
    *(SQLULEN *)ValuePtr= Stmt->Options.CursorType;
    break;
  case SQL_ATTR_CONCURRENCY:
    *(SQLULEN *)ValuePtr= SQL_CONCUR_READ_ONLY;
    break;
  case SQL_ATTR_ENABLE_AUTO_IPD:
    *(SQLULEN *)ValuePtr= SQL_FALSE;
    break;
  case SQL_ATTR_MAX_LENGTH:
    *(SQLULEN *)ValuePtr= Stmt->Options.MaxLength;
    break;
  case SQL_ATTR_MAX_ROWS:
    *(SQLULEN *)ValuePtr= Stmt->Options.MaxRows;
    break;
  case SQL_ATTR_METADATA_ID:
    /* SQL_ATTR_METADATA_ID is SQLUINTEGER attribute on connection level, but SQLULEN on statement level :/ */
    *(SQLULEN *)ValuePtr= Stmt->Options.MetadataId;
    break;
  case SQL_ATTR_NOSCAN:
    *(SQLULEN *)ValuePtr= SQL_NOSCAN_ON;
    break;
  case SQL_ATTR_QUERY_TIMEOUT:
    *(SQLULEN *)ValuePtr= Stmt->Options.Timeout;
    break;
  case SQL_ATTR_RETRIEVE_DATA:
    *(SQLULEN *)ValuePtr= SQL_RD_ON;
    break;
  }
  return ret;
}
/* }}} */


/* {{{ MADB_StmtSetAttr */
SQLRETURN MADB_StmtSetAttr(MADB_Stmt *Stmt, SQLINTEGER Attribute, SQLPOINTER ValuePtr, SQLINTEGER StringLength)
{
  SQLRETURN ret= SQL_SUCCESS;

  if (!Stmt)
    return SQL_INVALID_HANDLE;

  switch(Attribute) {
  case SQL_ATTR_APP_PARAM_DESC:
    if (ValuePtr)
    {
       MADB_Desc *Desc= (MADB_Desc *)ValuePtr;
      if (!Desc->AppType && Desc != Stmt->IApd)
      {
        MADB_SetError(&Stmt->Error, MADB_ERR_HY017, NULL, 0);
        return Stmt->Error.ReturnValue;
      }
      if (Desc->DescType != MADB_DESC_APD && Desc->DescType != MADB_DESC_UNKNOWN)
      {
        MADB_SetError(&Stmt->Error, MADB_ERR_HY024, NULL, 0);
        return Stmt->Error.ReturnValue;
      }
      RemoveStmtRefFromDesc(Stmt->Apd, Stmt, FALSE);
      Stmt->Apd= (MADB_Desc *)ValuePtr;
      Stmt->Apd->DescType= MADB_DESC_APD;
      if (Stmt->Apd != Stmt->IApd)
      {
        MADB_Stmt **IntStmt;
        IntStmt = (MADB_Stmt **)MADB_AllocDynamic(&Stmt->Apd->Stmts);
        *IntStmt= Stmt;
      }
    }
    else
    {
      RemoveStmtRefFromDesc(Stmt->Apd, Stmt, FALSE);
      Stmt->Apd= Stmt->IApd;
    }
    break;
  case SQL_ATTR_APP_ROW_DESC:
    if (ValuePtr)
    {
      MADB_Desc *Desc= (MADB_Desc *)ValuePtr;

      if (!Desc->AppType && Desc != Stmt->IArd)
      {
        MADB_SetError(&Stmt->Error, MADB_ERR_HY017, NULL, 0);
        return Stmt->Error.ReturnValue;
      }
      if (Desc->DescType != MADB_DESC_ARD && Desc->DescType != MADB_DESC_UNKNOWN)
      {
        MADB_SetError(&Stmt->Error, MADB_ERR_HY024, NULL, 0);
        return Stmt->Error.ReturnValue;
      }
      RemoveStmtRefFromDesc(Stmt->Ard, Stmt, FALSE);
      Stmt->Ard= Desc;
      Stmt->Ard->DescType= MADB_DESC_ARD;
      if (Stmt->Ard != Stmt->IArd)
      {
        MADB_Stmt **IntStmt;
        IntStmt = (MADB_Stmt **)MADB_AllocDynamic(&Stmt->Ard->Stmts);
        *IntStmt= Stmt;
      }
    }
    else
    {
      RemoveStmtRefFromDesc(Stmt->Ard, Stmt, FALSE);
      Stmt->Ard= Stmt->IArd;
    }
    break;

  case SQL_ATTR_PARAM_BIND_OFFSET_PTR:
    Stmt->Apd->Header.BindOffsetPtr= (SQLULEN*)ValuePtr;
    break;
  case SQL_ATTR_PARAM_BIND_TYPE:
    Stmt->Apd->Header.BindType= (SQLINTEGER)(SQLLEN)ValuePtr;
    break;
  case SQL_ATTR_PARAM_OPERATION_PTR:
    Stmt->Apd->Header.ArrayStatusPtr= (SQLUSMALLINT *)ValuePtr;
    break;
  case SQL_ATTR_PARAM_STATUS_PTR:
    Stmt->Ipd->Header.ArrayStatusPtr= (SQLUSMALLINT *)ValuePtr;
    break;
  case SQL_ATTR_PARAMS_PROCESSED_PTR:
    Stmt->Ipd->Header.RowsProcessedPtr  = (SQLULEN *)ValuePtr;
    break;
  case SQL_ATTR_PARAMSET_SIZE:
    Stmt->Apd->Header.ArraySize= (SQLULEN)ValuePtr;
    break;
  case SQL_ATTR_ROW_ARRAY_SIZE:
  case SQL_ROWSET_SIZE:
    Stmt->Ard->Header.ArraySize= (SQLULEN)ValuePtr;
    break;
  case SQL_ATTR_ROW_BIND_OFFSET_PTR:
    Stmt->Ard->Header.BindOffsetPtr= (SQLULEN*)ValuePtr;
    break;
  case SQL_ATTR_ROW_BIND_TYPE:
    Stmt->Ard->Header.BindType= (SQLINTEGER)(SQLLEN)ValuePtr;
    break;
  case SQL_ATTR_ROW_OPERATION_PTR:
    Stmt->Ard->Header.ArrayStatusPtr= (SQLUSMALLINT *)ValuePtr;
    break;
  case SQL_ATTR_ROW_STATUS_PTR:
    Stmt->Ird->Header.ArrayStatusPtr= (SQLUSMALLINT *)ValuePtr;
    break;
  case SQL_ATTR_ROWS_FETCHED_PTR:
    Stmt->Ird->Header.RowsProcessedPtr= (SQLULEN*)ValuePtr;
    break;
  case SQL_ATTR_ASYNC_ENABLE:
    if ((SQLULEN)ValuePtr != SQL_ASYNC_ENABLE_OFF)
    {
      MADB_SetError(&Stmt->Error, MADB_ERR_01S02, "Option value changed to default (SQL_ATTR_ASYNC_ENABLE)", 0);
      ret= SQL_SUCCESS_WITH_INFO;
    }
    break;
  case SQL_ATTR_SIMULATE_CURSOR:
    Stmt->Options.SimulateCursor= (SQLULEN) ValuePtr;
    break;
  case SQL_ATTR_CURSOR_SCROLLABLE:
    Stmt->Options.CursorType=  ((SQLULEN)ValuePtr == SQL_NONSCROLLABLE) ?
                               SQL_CURSOR_FORWARD_ONLY : SQL_CURSOR_STATIC;
    break;
  case SQL_ATTR_CURSOR_SENSITIVITY:
    /* we only support default value = SQL_UNSPECIFIED */
    if ((SQLULEN)ValuePtr != SQL_UNSPECIFIED)
    {
      MADB_SetError(&Stmt->Error, MADB_ERR_01S02, "Option value changed to default cursor sensitivity", 0);
      ret= SQL_SUCCESS_WITH_INFO;
    }
    break;
  case SQL_ATTR_CURSOR_TYPE:
    /* We need to check global DSN/Connection settings */
    if (MA_ODBC_CURSOR_FORWARD_ONLY(Stmt->Connection) && (SQLULEN)ValuePtr != SQL_CURSOR_FORWARD_ONLY)
    {
      MADB_SetError(&Stmt->Error, MADB_ERR_01S02, "Option value changed to default (SQL_CURSOR_FORWARD_ONLY)", 0);
      return Stmt->Error.ReturnValue;
    }
    else if (MA_ODBC_CURSOR_DYNAMIC(Stmt->Connection))
    {
      if ((SQLULEN)ValuePtr == SQL_CURSOR_KEYSET_DRIVEN)
      {
        Stmt->Options.CursorType= SQL_CURSOR_STATIC;
        MADB_SetError(&Stmt->Error, MADB_ERR_01S02, "Option value changed to default (SQL_CURSOR_STATIC)", 0);
        return Stmt->Error.ReturnValue;
      }
      Stmt->Options.CursorType= (SQLUINTEGER)(SQLULEN)ValuePtr;
    }
    /* only FORWARD or Static is allowed */
    else
    {
      if ((SQLULEN)ValuePtr != SQL_CURSOR_FORWARD_ONLY &&
          (SQLULEN)ValuePtr != SQL_CURSOR_STATIC)
      {
        Stmt->Options.CursorType= SQL_CURSOR_STATIC;
        MADB_SetError(&Stmt->Error, MADB_ERR_01S02, "Option value changed to default (SQL_CURSOR_STATIC)", 0);
        return Stmt->Error.ReturnValue;
      }
      Stmt->Options.CursorType= (SQLUINTEGER)(SQLULEN)ValuePtr;
    }
    break;
  case SQL_ATTR_CONCURRENCY:
    if ((SQLULEN)ValuePtr != SQL_CONCUR_READ_ONLY)
    {
      MADB_SetError(&Stmt->Error, MADB_ERR_01S02, "Option value changed to default (SQL_CONCUR_READ_ONLY). ", 0);
      ret= SQL_SUCCESS_WITH_INFO;
    }
    break;
  case SQL_ATTR_ENABLE_AUTO_IPD:
    /* MariaDB doesn't deliver param metadata after prepare, so we can't autopopulate ird */
    MADB_SetError(&Stmt->Error, MADB_ERR_HYC00, NULL, 0);
    return Stmt->Error.ReturnValue;
    break;
  case SQL_ATTR_MAX_LENGTH:
    Stmt->Options.MaxLength= (SQLULEN)ValuePtr;
    break;
  case SQL_ATTR_MAX_ROWS:
    Stmt->Options.MaxRows= (SQLULEN)ValuePtr;
    break;
  case SQL_ATTR_METADATA_ID:
    Stmt->Options.MetadataId= (SQLULEN)ValuePtr;
    break;
  case SQL_ATTR_NOSCAN:
    if ((SQLULEN)ValuePtr != SQL_NOSCAN_ON)
    {
       MADB_SetError(&Stmt->Error, MADB_ERR_01S02, "Option value changed to default (SQL_NOSCAN_ON)", 0);
       ret= SQL_SUCCESS_WITH_INFO;
    }
    break;
  case SQL_ATTR_QUERY_TIMEOUT:
    if (Stmt->Connection->IsMySQL)
    {
      return MADB_SetError(&Stmt->Error, MADB_ERR_01S02, "Option not supported with MySQL servers, value changed to default (0)", 0);
    }
    Stmt->Options.Timeout= (SQLULEN)ValuePtr;
    break;
  case SQL_ATTR_RETRIEVE_DATA:
    if ((SQLULEN)ValuePtr != SQL_RD_ON)
    {
       MADB_SetError(&Stmt->Error, MADB_ERR_01S02, "Option value changed to default (SQL_RD_ON)", 0);
       ret= SQL_SUCCESS_WITH_INFO;
    }
    break;
  case SQL_ATTR_USE_BOOKMARKS:
    Stmt->Options.UseBookmarks= (SQLUINTEGER)(SQLULEN)ValuePtr;
   break;
  case SQL_ATTR_FETCH_BOOKMARK_PTR:
    MADB_SetError(&Stmt->Error, MADB_ERR_HYC00, NULL, 0);
    return Stmt->Error.ReturnValue;
    break;
  default:
    MADB_SetError(&Stmt->Error, MADB_ERR_HY024, NULL, 0);
    return Stmt->Error.ReturnValue;
    break;
  }
  return ret;
}
/* }}} */

SQLRETURN MADB_GetBookmark(MADB_Stmt  *Stmt,
                           SQLSMALLINT TargetType,
                           SQLPOINTER  TargetValuePtr,
                           SQLLEN      BufferLength,
                           SQLLEN     *StrLen_or_IndPtr)
{
  if (Stmt->Options.UseBookmarks == SQL_UB_OFF)
  {
    MADB_SetError(&Stmt->Error, MADB_ERR_07009, NULL, 0);
    return Stmt->Error.ReturnValue;
  }

  if ((Stmt->Options.UseBookmarks == SQL_UB_VARIABLE && TargetType != SQL_C_VARBOOKMARK) ||
    (Stmt->Options.UseBookmarks != SQL_UB_VARIABLE && TargetType == SQL_C_VARBOOKMARK))
  {
    MADB_SetError(&Stmt->Error, MADB_ERR_HY003, NULL, 0);
    return Stmt->Error.ReturnValue;
  }

  if (TargetValuePtr && TargetType == SQL_C_BOOKMARK && BufferLength <= sizeof(SQLULEN))
  {
    *(SQLULEN *)TargetValuePtr= Stmt->Cursor.Position;
    if (StrLen_or_IndPtr)
    {
      *StrLen_or_IndPtr= sizeof(SQLULEN);
    }
    return SQL_SUCCESS;
  }
  /* Keeping compiler happy */
  return SQL_SUCCESS;
}

/* {{{ MADB_StmtGetData */
SQLRETURN MADB_StmtGetData(SQLHSTMT StatementHandle,
                           SQLUSMALLINT Col_or_Param_Num,
                           SQLSMALLINT TargetType,
                           SQLPOINTER TargetValuePtr,
                           SQLLEN BufferLength,
                           SQLLEN * StrLen_or_IndPtr,
                           BOOL   InternalUse /* Currently this is respected for SQL_CHAR type only,
                                                 since all "internal" calls of the function need string representation of datat */)
{
  MADB_Stmt       *Stmt= (MADB_Stmt *)StatementHandle;
  SQLUSMALLINT    Offset= Col_or_Param_Num - 1;
  SQLSMALLINT     OdbcType = 0;
  enum enum_field_types MadbType = MYSQL_TYPE_DECIMAL;
  MYSQL_BIND      Bind;
  my_bool         IsNull= FALSE;
  my_bool         ZeroTerminated= 0;
  unsigned long   CurrentOffset= InternalUse == TRUE ? 0 : Stmt->CharOffset[Offset]; /* We are supposed not get bookmark column here */
  my_bool         Error;
  MADB_DescRecord *IrdRec= nullptr;
  const MYSQL_FIELD *Field= Stmt->metadata->getField(Offset);

  MADB_CLEAR_ERROR(&Stmt->Error);

  /* Should not really happen, and is evidence of that something wrong happened in some previous call(SQLFetch?)
   * But do we really need this paranoid here?
   */
  if (Stmt->result == nullptr)
  {
    return MADB_SetError(&Stmt->Error, MADB_ERR_HY109, NULL, 0);
  }
  /* Will it be set with all dummies? */
  if (Stmt->result[Offset].is_null != NULL && *Stmt->result[Offset].is_null != '\0')
  {
    if (!StrLen_or_IndPtr)
    {
      return MADB_SetError(&Stmt->Error, MADB_ERR_22002, NULL, 0);
    }
    *StrLen_or_IndPtr= SQL_NULL_DATA;
    return SQL_SUCCESS;
  }

  memset(&Bind, 0, sizeof(MYSQL_BIND));

  /* We might need it for SQL_C_DEFAULT type, or to obtain length of fixed length types(Access likes to have it) */
  IrdRec= MADB_DescGetInternalRecord(Stmt->Ird, Offset, MADB_DESC_READ);
  if (!IrdRec)
  {
    MADB_SetError(&Stmt->Error, MADB_ERR_07009, NULL, 0);
    return Stmt->Error.ReturnValue;
  }

  switch (TargetType) {
  case SQL_ARD_TYPE:
    {
      MADB_DescRecord *Ard= MADB_DescGetInternalRecord(Stmt->Ard, Offset, MADB_DESC_READ);

      if (!Ard)
      {
        MADB_SetError(&Stmt->Error, MADB_ERR_07009, NULL, 0);
        return Stmt->Error.ReturnValue;
      }
      OdbcType= Ard->ConciseType;
    }
    break;
  case SQL_C_DEFAULT:
    {
      /* Taking type from IRD record. This way, if mysql type was fixed(currently that is mainly for catalog functions, we don't lose it.
         (Access uses default types on getting catalog functions results, and not quite happy when it gets something unexpected. Seemingly it cares about returned data lenghts even for types,
         for which standard says application should not care about */
      OdbcType= IrdRec->ConciseType;
    }
    break;
  default:
    OdbcType= TargetType;
    break;  
  }
  /* Restoring mariadb/mysql type from odbc type */
  MadbType= MADB_GetMaDBTypeAndLength(OdbcType, &Bind.is_unsigned, &Bind.buffer_length);

  /* set global values for Bind */
  Bind.error=   &Error;
  Bind.length=  &Bind.length_value;
  Bind.is_null= &IsNull;

  try
  {
    switch (OdbcType)
    {
    case SQL_DATE:
    case SQL_C_TYPE_DATE:
    case SQL_TIMESTAMP:
    case SQL_C_TYPE_TIMESTAMP:
    case SQL_TIME:
    case SQL_C_TYPE_TIME:
    {
      MYSQL_TIME tm;

      if (IrdRec->ConciseType == SQL_CHAR || IrdRec->ConciseType == SQL_VARCHAR)
      {
        char  *ClientValue= NULL;
        BOOL isTime;
        Bind.buffer_length= (Field->max_length != 0 ? Field->max_length : Field->length) + 1;

        if (IrdRec->InternalBuffer != NULL)
        {
          IrdRec->InternalBuffer = (char*)MADB_REALLOC(IrdRec->InternalBuffer, Bind.buffer_length);
        }
        else
        {
          IrdRec->InternalBuffer= (char*)MADB_ALLOC(Bind.buffer_length);
        }
          
        if (IrdRec->InternalBuffer == NULL)
        {
          return MADB_SetError(&Stmt->Error, MADB_ERR_HY001, NULL, 0);
        }
        Bind.buffer=        IrdRec->InternalBuffer;
        Bind.buffer_type=   MYSQL_TYPE_STRING;
        Stmt->rs->get(&Bind, Offset, 0);
        RETURN_ERROR_OR_CONTINUE(MADB_Str2Ts(IrdRec->InternalBuffer, Bind.length_value, &tm, FALSE, &Stmt->Error, &isTime));
      }
      else
      {
        Bind.buffer_length= sizeof(MYSQL_TIME);
        Bind.buffer= (void *)&tm;
        /* c/c is too smart to convert hours to days and days to hours, we don't need that */
        if ((OdbcType == SQL_C_TIME || OdbcType == SQL_C_TYPE_TIME)
          && (IrdRec->ConciseType == SQL_TIME || IrdRec->ConciseType == SQL_TYPE_TIME))
        {
          Bind.buffer_type = MYSQL_TYPE_TIME;
        }
        else
        {
          Bind.buffer_type = MYSQL_TYPE_TIMESTAMP;

        }
        Stmt->rs->get(&Bind, Offset, 0);
      }
      RETURN_ERROR_OR_CONTINUE(MADB_CopyMadbTimestamp(Stmt, &tm, TargetValuePtr, StrLen_or_IndPtr, StrLen_or_IndPtr, OdbcType, IrdRec->ConciseType));
      break;
    }
    case SQL_C_INTERVAL_HOUR_TO_MINUTE:
    case SQL_C_INTERVAL_HOUR_TO_SECOND:
    {
      MYSQL_TIME tm;
      SQL_INTERVAL_STRUCT* ts = (SQL_INTERVAL_STRUCT*)TargetValuePtr;

      if (IrdRec->ConciseType == SQL_CHAR || IrdRec->ConciseType == SQL_VARCHAR)
      {
        BOOL isTime;

        Bind.buffer_length = (Field->max_length != 0 ? Field->max_length :
          Field->length) + 1;

        if (IrdRec->InternalBuffer != NULL)
        {
          IrdRec->InternalBuffer = (char*)MADB_REALLOC(IrdRec->InternalBuffer, Bind.buffer_length);
        }
        else
        {
          IrdRec->InternalBuffer = (char*)MADB_ALLOC(Bind.buffer_length);
        }

        if (IrdRec->InternalBuffer == NULL)
        {
          return MADB_SetError(&Stmt->Error, MADB_ERR_HY001, NULL, 0);
        }
        Bind.buffer=        IrdRec->InternalBuffer;
        Bind.buffer_type=   MYSQL_TYPE_STRING;
        Stmt->rs->get(&Bind, Offset, 0);
        RETURN_ERROR_OR_CONTINUE(MADB_Str2Ts(IrdRec->InternalBuffer, Bind.length_value, &tm, TRUE, &Stmt->Error, &isTime));
      }
      else
      {
        Bind.buffer_length = sizeof(MYSQL_TIME);
        Bind.buffer = (void*)&tm;
        /* c/c is too smart to convert hours to days and days to hours, we don't need that */
        Bind.buffer_type = Field && Field->type == MYSQL_TYPE_TIME ? MYSQL_TYPE_TIME : MYSQL_TYPE_TIMESTAMP;
        Stmt->rs->get(&Bind, Offset, 0);
      }

      if (tm.hour > 99999)
      {
        return MADB_SetError(&Stmt->Error, MADB_ERR_22015, NULL, 0);
      }

      ts->intval.day_second.hour = tm.hour;
      ts->intval.day_second.minute = tm.minute;
      ts->interval_sign = tm.neg ? SQL_TRUE : SQL_FALSE;

      if (TargetType == SQL_C_INTERVAL_HOUR_TO_MINUTE)
      {
        ts->intval.day_second.second = 0;
        ts->interval_type = /*SQLINTERVAL::*/SQL_IS_HOUR_TO_MINUTE;
        if (tm.second)
        {
          return MADB_SetError(&Stmt->Error, MADB_ERR_01S07, NULL, 0);
        }
      }
      else
      {
        ts->interval_type = /*SQLINTERVAL::*/SQL_IS_HOUR_TO_SECOND;
        ts->intval.day_second.second = tm.second;
      }
      if (StrLen_or_IndPtr)
      {
        *StrLen_or_IndPtr = sizeof(SQL_INTERVAL_STRUCT);
      }
    }
    break;
    case SQL_WCHAR:
    case SQL_WVARCHAR:
    case SQL_WLONGVARCHAR:
    {
      char* ClientValue = NULL;
      size_t CharLength = 0;

      /* Kinda this it not 1st call for this value, and we have it nice and recoded */
      if (IrdRec->InternalBuffer == NULL/* && Stmt->Lengths[Offset] == 0*/)
      {
        unsigned long FieldBufferLen = 0;
        Bind.length = &FieldBufferLen;
        Bind.buffer_type = MYSQL_TYPE_STRING;
        /* Getting value's length to allocate the buffer */
        if (Stmt->rs->get(&Bind, Offset, Stmt->CharOffset[Offset]))
        {
          return MADB_SetNativeError(&Stmt->Error, SQL_HANDLE_STMT, Stmt->stmt.get());
        }
        /* Adding byte for terminating null */
        ++FieldBufferLen;
        if (!(ClientValue = (char*)MADB_CALLOC(FieldBufferLen)))
        {
          return MADB_SetError(&Stmt->Error, MADB_ERR_HY001, NULL, 0);
        }
        Bind.buffer = ClientValue;
        Bind.buffer_length = FieldBufferLen;
        Bind.length = &Bind.length_value;

        if (Stmt->rs->get(&Bind, Offset, Stmt->CharOffset[Offset]))
        {
          return MADB_SetNativeError(&Stmt->Error, SQL_HANDLE_STMT, Stmt->stmt.get());
        }

        /* check total length: if not enough space, we need to calculate new CharOffset for next fetch */
        if (Bind.length_value > 0)
        {
          size_t ReqBuffOctetLen;
          /* Size in chars */
          CharLength = MbstrCharLen(ClientValue, Bind.length_value - Stmt->CharOffset[Offset],
            Stmt->Connection->Charset.cs_info);
          /* MbstrCharLen gave us length in characters. For encoding of each character we might need
             2 SQLWCHARs in case of UTF16, or 1 SQLWCHAR in case of UTF32 = 4 bytes in each case. Probably we need calcualate better
             number of required SQLWCHARs */
          ReqBuffOctetLen = (CharLength + 1) * 4;

          if (BufferLength)
          {
            /* Buffer is not big enough. Alocating InternalBuffer.
               MADB_SetString would do that anyway if - allocate buffer fitting the whole wide string,
               and then copied its part to the application's buffer */
            if (ReqBuffOctetLen > (size_t)BufferLength)
            {
              IrdRec->InternalBuffer = (char*)MADB_CALLOC(ReqBuffOctetLen);

              if (IrdRec->InternalBuffer == 0)
              {
                MADB_FREE(ClientValue);
                return MADB_SetError(&Stmt->Error, MADB_ERR_HY001, NULL, 0);
              }

              CharLength = MADB_SetString(&Stmt->Connection->Charset, IrdRec->InternalBuffer, (SQLINTEGER)ReqBuffOctetLen / sizeof(SQLWCHAR),
                ClientValue, Bind.length_value - Stmt->CharOffset[Offset], &Stmt->Error);
            }
            else
            {
              /* Application's buffer is big enough - writing directly there */
              CharLength = MADB_SetString(&Stmt->Connection->Charset, TargetValuePtr, (SQLINTEGER)(BufferLength / sizeof(SQLWCHAR)),
                ClientValue, Bind.length_value - Stmt->CharOffset[Offset], &Stmt->Error);
            }

            if (!SQL_SUCCEEDED(Stmt->Error.ReturnValue))
            {
              MADB_FREE(ClientValue);
              MADB_FREE(IrdRec->InternalBuffer);

              return Stmt->Error.ReturnValue;
            }
          }

          if (!Stmt->CharOffset[Offset])
          {
            Stmt->Lengths[Offset] = (unsigned long)(CharLength * sizeof(SQLWCHAR));
          }
        }
        else if (BufferLength >= sizeof(SQLWCHAR))
        {
          *(SQLWCHAR*)TargetValuePtr = 0;
        }
      }
      else  /* IrdRec->InternalBuffer == NULL && Stmt->Lengths[Offset] == 0 */
      {
        CharLength = SqlwcsLen((SQLWCHAR*)((char*)IrdRec->InternalBuffer + Stmt->CharOffset[Offset]), -1);
      }

      if (StrLen_or_IndPtr)
      {
        *StrLen_or_IndPtr = CharLength * sizeof(SQLWCHAR);
      }

      if (!BufferLength)
      {
        MADB_FREE(ClientValue);

        return MADB_SetError(&Stmt->Error, MADB_ERR_01004, NULL, 0);
      }

      if (IrdRec->InternalBuffer)
      {
        /* If we have more place than only for the TN */
        if (BufferLength > sizeof(SQLWCHAR))
        {
          memcpy(TargetValuePtr, (char*)IrdRec->InternalBuffer + Stmt->CharOffset[Offset],
            MIN(BufferLength - sizeof(SQLWCHAR), CharLength * sizeof(SQLWCHAR)));
        }
        /* Terminating Null */
        *(SQLWCHAR*)((char*)TargetValuePtr + MIN(BufferLength - sizeof(SQLWCHAR), CharLength * sizeof(SQLWCHAR))) = 0;
      }

      if (CharLength >= BufferLength / sizeof(SQLWCHAR))
      {
        /* Calculate new offset and substract 1 byte for null termination */
        Stmt->CharOffset[Offset] += (unsigned long)BufferLength - sizeof(SQLWCHAR);
        MADB_FREE(ClientValue);

        return MADB_SetError(&Stmt->Error, MADB_ERR_01004, NULL, 0);
      }
      else
      {
        Stmt->CharOffset[Offset] = Stmt->Lengths[Offset];
        MADB_FREE(IrdRec->InternalBuffer);
      }

      MADB_FREE(ClientValue);
    }
    break;
    case SQL_CHAR:
    case SQL_VARCHAR:
      if (Field->type == MYSQL_TYPE_BLOB && Field->charsetnr == 63)
      {
        if (!BufferLength && StrLen_or_IndPtr)
        {
          Bind.buffer = NULL;
          Bind.buffer_length = 0;
          Stmt->rs->get(&Bind, Offset, 0);
          *StrLen_or_IndPtr = Bind.length_value * 2;
          return SQL_SUCCESS_WITH_INFO;
        }

#ifdef CONVERSION_TO_HEX_IMPLEMENTED
        {
          /*TODO: */
          char* TmpBuffer;
          if (!(TmpBuffer = (char*)MADB_CALLOC(BufferLength)))
          {

          }
        }
#endif
      }
      ZeroTerminated = 1;

    case SQL_LONGVARCHAR:
    case SQL_BINARY:
    case SQL_VARBINARY:
    case SQL_LONGVARBINARY:
    {
      Bind.buffer = TargetValuePtr;
      Bind.buffer_length = (unsigned long)BufferLength;
      Bind.buffer_type = MadbType;

      if (!(BufferLength) && StrLen_or_IndPtr)
      {
        /* Paranoid - before StrLen_or_IndPtr was used as length directly. so leaving same value in Bind.length. Unlikely needed */
        Bind.length_value = (unsigned long)*StrLen_or_IndPtr;
        Bind.length = &Bind.length_value;

        Stmt->rs->get(&Bind, Offset, Stmt->CharOffset[Offset]);

        if (InternalUse)
        {
          *StrLen_or_IndPtr = *Bind.length;//MIN(*Bind.length, Stmt->stmt->fields[Offset].max_length);
        }
        else
        {
          if (!Stmt->CharOffset[Offset])
          {
            Stmt->Lengths[Offset] = *Bind.length;//MIN(*Bind.length, Stmt->stmt->fields[Offset].max_length);
          }
          *StrLen_or_IndPtr = Stmt->Lengths[Offset] - Stmt->CharOffset[Offset];
        }

        MADB_SetError(&Stmt->Error, MADB_ERR_01004, NULL, 0);

        return SQL_SUCCESS_WITH_INFO;
      }

      if (Stmt->rs->get(&Bind, Offset, CurrentOffset))
      {
        MADB_SetNativeError(&Stmt->Error, SQL_HANDLE_STMT, Stmt->stmt.get());
        return Stmt->Error.ReturnValue;
      }
      /* Dirty temporary hack before we know what is going on. Yes, there is nothing more eternal, than temporary
         It's not that bad, after all */
      if ((long)*Bind.length == -1)
      {
        *Bind.length = 0;
      }
      /* end of dirty hack */

      if (!InternalUse && !Stmt->CharOffset[Offset])
      {
        Stmt->Lengths[Offset] = *Bind.length; // MIN(*Bind.length, Stmt->stmt->fields[Offset].max_length);
      }
      if (ZeroTerminated)
      {
        char* p = (char*)Bind.buffer;
        if (BufferLength > (SQLLEN)*Bind.length)
        {
          p[*Bind.length] = 0;
        }
        else
        {
          p[BufferLength - 1] = 0;
        }
      }

      if (StrLen_or_IndPtr)
      {
        *StrLen_or_IndPtr = *Bind.length - CurrentOffset;
      }
      if (InternalUse == FALSE)
      {
        /* Recording new offset only if that is API call, and not getting data for internal use */
        Stmt->CharOffset[Offset] += MIN((unsigned long)BufferLength - ZeroTerminated, *Bind.length);
        if ((BufferLength - ZeroTerminated) && Stmt->Lengths[Offset] > Stmt->CharOffset[Offset])
        {
          return MADB_SetError(&Stmt->Error, MADB_ERR_01004, NULL, 0);
        }
      }

      if (StrLen_or_IndPtr && BufferLength - ZeroTerminated < *StrLen_or_IndPtr)
      {
        return MADB_SetError(&Stmt->Error, MADB_ERR_01004, NULL, 0);
      }
    }
    break;
    case SQL_NUMERIC:
    {
      SQLRETURN rc;
      char *tmp= NULL;
      MADB_DescRecord *Ard= MADB_DescGetInternalRecord(Stmt->Ard, Offset, MADB_DESC_READ);

      Bind.buffer_length= MADB_DEFAULT_PRECISION + 1/*-*/ + 1/*.*/;
      if (IrdRec->InternalBuffer != NULL)
      {
        tmp= (char*)MADB_REALLOC(IrdRec->InternalBuffer, Bind.buffer_length);
      }
      else
      {
        tmp= (char*)MADB_ALLOC(Bind.buffer_length);
      }

      if (tmp == NULL)
      {
        return MADB_SetError(&Stmt->Error, MADB_ERR_HY001, NULL, 0);
      }
      else
      {
        IrdRec->InternalBuffer= tmp;
      }
      Bind.buffer=        IrdRec->InternalBuffer;
      Bind.buffer_type=   MadbType;

      Stmt->rs->get(&Bind, Offset, 0);

      MADB_CLEAR_ERROR(&Stmt->Error);

      if (Bind.buffer_length < *Bind.length)
      {
        MADB_SetError(&Stmt->Error, MADB_ERR_22003, NULL, 0);
        return Stmt->Error.ReturnValue;
      }

      rc= MADB_CharToSQLNumeric(IrdRec->InternalBuffer, Stmt->Ard, Ard, static_cast<SQL_NUMERIC_STRUCT*>(TargetValuePtr), 0);

      /* Ugly */
      if (rc != SQL_SUCCESS)
      {
        MADB_SetError(&Stmt->Error, rc, NULL, 0);
        if (rc == SQL_ERROR)
        {
          return SQL_ERROR;
        }
      }

      if (StrLen_or_IndPtr != NULL)
      {
        *StrLen_or_IndPtr = sizeof(SQL_NUMERIC_STRUCT);
      }
      break;
    }
    default:
    {
      /* Set the conversion function */
      Bind.buffer_type = MadbType;
      Bind.buffer = TargetValuePtr;
      if (Bind.buffer_length == 0 && BufferLength > 0)
      {
        Bind.buffer_length = (unsigned long)BufferLength;
      }
      Stmt->rs->get(&Bind, Offset, 0);

      if (StrLen_or_IndPtr != NULL)
      {
        /* We get here only for fixed data types. Thus, according to the specs
           "this is the length of the data after conversion; that is, it is the size of the type to which the data was converted".
           For us that is the size of the buffer in bind structure. Not the size of the field */

        *StrLen_or_IndPtr = Bind.buffer_length;

        /* Paranoid - it was here, so leaving it in place */
        if ((long)Bind.length_value == -1)
        {
          Bind.length_value = 0;
        }
        /* We do this for catalog functions and MS Access in first turn. The thing is that for some columns in catalog functions result,
           we fix column type manually, since we can't make field of desired type in the query to I_S. Mostly that is for SQLSMALLINT
           fields, and we can cast only to int, not to short. MSAccess in its turn like to to get length for fixed length types, and
           throws error if the length is not what it expected (ODBC-131)
           Probably it makes sense to do this only for SQL_C_DEFAULT type, which MS Access uses. But atm it looks like this should
           not hurt if done for other types, too */
        if (*StrLen_or_IndPtr == 0 || (TargetType == SQL_C_DEFAULT && Bind.length_value > (unsigned long)IrdRec->OctetLength && *StrLen_or_IndPtr > IrdRec->OctetLength))
        {
          *StrLen_or_IndPtr = IrdRec->OctetLength;
        }
      }
    }
    }             /* End of switch(OdbcType) */
  }
  catch (SQLException& e) {
    return MADB_FromException(Stmt->Error, e);
  }
  catch (int32_t /*rc*/) {
    return MADB_SetNativeError(&Stmt->Error, SQL_HANDLE_STMT, Stmt->stmt.get());
  }
  catch (std::invalid_argument &ia) {
    return MADB_SetError(&Stmt->Error, MADB_ERR_22018, ia.what(), 0);
  }
  catch (std::out_of_range &oor) {
    return MADB_SetError(&Stmt->Error, MADB_ERR_22003, oor.what(), 0);
  }

  /* Marking fixed length fields to be able to return SQL_NO_DATA on subsequent calls, as standard prescribes
     "SQLGetData cannot be used to return fixed-length data in parts. If SQLGetData is called more than one time
      in a row for a column containing fixed-length data, it returns SQL_NO_DATA for all calls after the first."
     Stmt->Lengths[Offset] would be set for variable length types */
  if (!InternalUse && Stmt->Lengths[Offset] == 0)
  {
    Stmt->CharOffset[Offset]= MAX((unsigned long)Bind.buffer_length, Bind.length_value);
  }

  if (IsNull)
  {
    if (!StrLen_or_IndPtr)
    {
      return MADB_SetError(&Stmt->Error, MADB_ERR_22002, NULL, 0);
    }
    *StrLen_or_IndPtr= SQL_NULL_DATA;
  }

  return Stmt->Error.ReturnValue;
}
/* }}} */

/* {{{ MADB_StmtRowCount */
SQLRETURN MADB_StmtRowCount(MADB_Stmt *Stmt, SQLLEN *RowCountPtr)
{
  if (Stmt->AffectedRows != -1)
    *RowCountPtr= (SQLLEN)Stmt->AffectedRows;
  else if (Stmt->rs)
  {
    if (MADB_STMT_IS_STREAMING(Stmt))
    {
      LOCK_MARIADB(Stmt->Connection);
      if (MADB_STMT_IS_STREAMING(Stmt))
      {
        // TODO: return value has to be checked?
        Stmt->Connection->Methods->CacheRestOfCurrentRsStream(Stmt->Connection, &Stmt->Error);
      }
      UNLOCK_MARIADB(Stmt->Connection);
    }
    *RowCountPtr = (SQLLEN)(Stmt->rs->rowsCount());
  }
  else
    *RowCountPtr= 0;
  return SQL_SUCCESS;
}
/* }}} */

/* {{{ MapColAttributesDescType */
SQLUSMALLINT MapColAttributeDescType(SQLUSMALLINT FieldIdentifier)
{
  /* we need to map the old field identifiers, see bug ODBC-8 */
  switch (FieldIdentifier)
  {
  case SQL_COLUMN_SCALE:
    return SQL_DESC_SCALE;
  case SQL_COLUMN_PRECISION:
    return SQL_DESC_PRECISION;
  case SQL_COLUMN_NULLABLE:
    return SQL_DESC_NULLABLE;
  case SQL_COLUMN_LENGTH:
    return SQL_DESC_OCTET_LENGTH;
  case SQL_COLUMN_NAME:
    return SQL_DESC_NAME;
  default:
    return FieldIdentifier;
  }
}
/* }}} */

/* {{{ MADB_StmtRowCount */
SQLRETURN MADB_StmtParamCount(MADB_Stmt *Stmt, SQLSMALLINT *ParamCountPtr)
{
  *ParamCountPtr= static_cast<SQLSMALLINT>(Stmt->stmt->getParamCount());
  return SQL_SUCCESS;
}
/* }}} */

/* {{{ MADB_StmtColumnCount */
SQLRETURN MADB_StmtColumnCount(MADB_Stmt *Stmt, SQLSMALLINT *ColumnCountPtr)
{
  /* We supposed to have that data in the descriptor by now. No sense to ask C/C API one more time for that */
  *ColumnCountPtr= (SQLSMALLINT)MADB_STMT_COLUMN_COUNT(Stmt);
  return SQL_SUCCESS;
}
/* }}} */

/* {{{ MADB_StmtColAttr */
SQLRETURN MADB_StmtColAttr(MADB_Stmt *Stmt, SQLUSMALLINT ColumnNumber, SQLUSMALLINT FieldIdentifier, SQLPOINTER CharacterAttributePtr,
             SQLSMALLINT BufferLength, SQLSMALLINT *StringLengthPtr, SQLLEN *NumericAttributePtr, my_bool IsWchar)
{
  MADB_DescRecord *Record;
  SQLSMALLINT     StringLength=     0;
  SQLLEN          NumericAttribute;
  BOOL            IsNumericAttr=    TRUE;

  if (!Stmt)
    return SQL_INVALID_HANDLE;
  
  MADB_CLEAR_ERROR(&Stmt->Error);

  if (StringLengthPtr)
    *StringLengthPtr= 0;

  if (!Stmt->rs)
  {
    MADB_SetError(&Stmt->Error, MADB_ERR_07005, NULL, 0);
    return Stmt->Error.ReturnValue;
  }

  if (ColumnNumber < 1 || ColumnNumber > Stmt->metadata->getColumnCount())
  {
    MADB_SetError(&Stmt->Error, MADB_ERR_07009, NULL, 0);
    return Stmt->Error.ReturnValue;
  }

  /* We start at offset zero */
  --ColumnNumber;

  if (!(Record= MADB_DescGetInternalRecord(Stmt->Ird, ColumnNumber, MADB_DESC_READ)))
  {
    MADB_SetError(&Stmt->Error, MADB_ERR_07009, NULL, 0);
    return Stmt->Error.ReturnValue;
  }

  /* Mapping ODBC2 attributes to ODBC3
     TODO: it looks like it takes more than that 
     "In ODBC 3.x driver must support SQL_COLUMN_PRECISION and SQL_DESC_PRECISION, SQL_COLUMN_SCALE and SQL_DESC_SCALE,
     and SQL_COLUMN_LENGTH and SQL_DESC_LENGTH. These values are different because precision, scale, and length are defined
     differently in ODBC 3.x than they were in ODBC 2.x."
     */
  FieldIdentifier= MapColAttributeDescType(FieldIdentifier);

  switch(FieldIdentifier) {
  case 1212/* SQL_COLUMN_AUTO_INCREMENT - not part of ODBC specs, but used by many systems. In particular can be seen in Access
              traces(so must be MS thing) and in Embarcadero generated code */:
  case SQL_DESC_AUTO_UNIQUE_VALUE:
    NumericAttribute= (SQLLEN)Record->AutoUniqueValue != 0 ? SQL_TRUE : SQL_FALSE;
    break;
  case SQL_DESC_BASE_COLUMN_NAME:
    StringLength= (SQLSMALLINT)MADB_SetString(IsWchar ? &Stmt->Connection->Charset : NULL,
                                              CharacterAttributePtr, (IsWchar) ? BufferLength / sizeof(SQLWCHAR) : BufferLength,
                                              Record->BaseColumnName, strlen(Record->BaseColumnName), &Stmt->Error);
    IsNumericAttr= FALSE;
    break;
  case SQL_DESC_BASE_TABLE_NAME:
    StringLength= (SQLSMALLINT)MADB_SetString(IsWchar ? &Stmt->Connection->Charset : NULL,
                                              CharacterAttributePtr, (IsWchar) ? BufferLength / sizeof(SQLWCHAR) : BufferLength,
                                              Record->BaseTableName, strlen(Record->BaseTableName), &Stmt->Error);
    IsNumericAttr= FALSE;
    break;
  case SQL_DESC_CASE_SENSITIVE:
    NumericAttribute= (SQLLEN)Record->CaseSensitive;
    break;
  case SQL_DESC_CATALOG_NAME:
    StringLength= (SQLSMALLINT)MADB_SetString(IsWchar ? &Stmt->Connection->Charset : 0,
                                              CharacterAttributePtr, (IsWchar) ? BufferLength / sizeof(SQLWCHAR) : BufferLength,
                                              Record->CatalogName, strlen(Record->CatalogName), &Stmt->Error);
    IsNumericAttr= FALSE;
    break;
  case SQL_DESC_SCHEMA_NAME:
    StringLength= (SQLSMALLINT)MADB_SetString(IsWchar ? &Stmt->Connection->Charset : 0,
                                              CharacterAttributePtr, (IsWchar) ? BufferLength / sizeof(SQLWCHAR) : BufferLength,
                                              "", 0, &Stmt->Error);
    IsNumericAttr= FALSE;
  case SQL_DESC_CONCISE_TYPE:
    NumericAttribute= (SQLLEN)Record->ConciseType;
    break;
  case SQL_DESC_SEARCHABLE:
    NumericAttribute= (SQLLEN)Record->Searchable;
    break;
  case SQL_DESC_COUNT:
    NumericAttribute= (SQLLEN)Stmt->Ird->Header.Count;
    break;
  case SQL_DESC_DISPLAY_SIZE:
    NumericAttribute= (SQLLEN)Record->DisplaySize;
    break;
  case SQL_DESC_FIXED_PREC_SCALE:
    NumericAttribute= (SQLLEN)Record->FixedPrecScale;
    break;
  case SQL_DESC_PRECISION:
    NumericAttribute= (SQLLEN)Record->Precision;
    break;
  case SQL_DESC_LENGTH:
    NumericAttribute= (SQLLEN)Record->Length;
    break;
  case SQL_DESC_LITERAL_PREFIX:
    StringLength= (SQLSMALLINT)MADB_SetString(IsWchar ? &Stmt->Connection->Charset : 0,
                                              CharacterAttributePtr, (IsWchar) ? BufferLength / sizeof(SQLWCHAR) : BufferLength,
                                              Record->LiteralPrefix, strlen(Record->LiteralPrefix), &Stmt->Error);
    IsNumericAttr= FALSE;
    break;
  case SQL_DESC_LITERAL_SUFFIX:
    StringLength= (SQLSMALLINT)MADB_SetString(IsWchar ? &Stmt->Connection->Charset : 0,
                                              CharacterAttributePtr, (IsWchar) ? BufferLength / sizeof(SQLWCHAR) : BufferLength,
                                              Record->LiteralSuffix, strlen(Record->LiteralSuffix), &Stmt->Error);
    IsNumericAttr= FALSE;
    break;
  case SQL_DESC_LOCAL_TYPE_NAME:
    StringLength= (SQLSMALLINT)MADB_SetString(IsWchar ? &Stmt->Connection->Charset : 0,
                                              CharacterAttributePtr, (IsWchar) ? BufferLength / sizeof(SQLWCHAR) : BufferLength,
                                              "", 0, &Stmt->Error);
    IsNumericAttr= FALSE;
    break;
  case SQL_DESC_LABEL:
  case SQL_DESC_NAME:
    StringLength= (SQLSMALLINT)MADB_SetString(IsWchar ? &Stmt->Connection->Charset : 0,
                                              CharacterAttributePtr, (IsWchar) ? BufferLength / sizeof(SQLWCHAR) : BufferLength,
                                              Record->ColumnName, strlen(Record->ColumnName), &Stmt->Error);
    IsNumericAttr= FALSE;
    break;
  case SQL_DESC_TYPE_NAME:
    StringLength= (SQLSMALLINT)MADB_SetString(IsWchar ? &Stmt->Connection->Charset : 0,
                                              CharacterAttributePtr, (IsWchar) ? BufferLength / sizeof(SQLWCHAR) : BufferLength,
                                              Record->TypeName, strlen(Record->TypeName), &Stmt->Error);
    IsNumericAttr= FALSE;
    break;
  case SQL_DESC_NULLABLE:
    NumericAttribute= Record->Nullable;
    break;
  case SQL_DESC_UNNAMED:
    NumericAttribute= Record->Unnamed;
    break;
  case SQL_DESC_UNSIGNED:
    NumericAttribute= Record->Unsigned;
    break;
  case SQL_DESC_UPDATABLE:
    NumericAttribute= Record->Updateable;
    break;
  case SQL_DESC_OCTET_LENGTH:
    NumericAttribute= Record->OctetLength;
    break;
  case SQL_DESC_SCALE:
    NumericAttribute= Record->Scale;
    break;
  case SQL_DESC_TABLE_NAME:
    StringLength= (SQLSMALLINT)MADB_SetString(IsWchar ? &Stmt->Connection->Charset : 0,
                                              CharacterAttributePtr, (IsWchar) ? BufferLength / sizeof(SQLWCHAR) : BufferLength,
                                              Record->TableName, strlen(Record->TableName), &Stmt->Error);
    IsNumericAttr= FALSE;
    break;
  case SQL_DESC_TYPE:
    NumericAttribute= Record->Type;
    break;
  case SQL_COLUMN_COUNT:
    NumericAttribute= Stmt->metadata->getColumnCount();
    break;
  default:
    MADB_SetError(&Stmt->Error, MADB_ERR_HYC00, NULL, 0);
    return Stmt->Error.ReturnValue;
  }
  /* We need to return the number of bytes, not characters! */
  if (StringLength)
  {
    if (StringLengthPtr)
      *StringLengthPtr= (SQLSMALLINT)StringLength;
    if (!BufferLength && CharacterAttributePtr)
      MADB_SetError(&Stmt->Error, MADB_ERR_01004, NULL, 0);
  }
  /* We shouldn't touch application memory without purpose, writing garbage there. Thus IsNumericAttr.
     Besides .Net was quite disappointed about that */
  if (NumericAttributePtr && IsNumericAttr == TRUE)
    *NumericAttributePtr= NumericAttribute;
  if (StringLengthPtr && IsWchar)
    *StringLengthPtr*= sizeof(SQLWCHAR);
  return Stmt->Error.ReturnValue;
}
/* }}} */

/* {{{ MADB_StmtDescribeCol */
SQLRETURN MADB_StmtDescribeCol(MADB_Stmt *Stmt, SQLUSMALLINT ColumnNumber, void *ColumnName,
                         SQLSMALLINT BufferLength, SQLSMALLINT *NameLengthPtr,
                         SQLSMALLINT *DataTypePtr, SQLULEN *ColumnSizePtr, SQLSMALLINT *DecimalDigitsPtr,
                         SQLSMALLINT *NullablePtr, my_bool isWChar)
{
  MADB_DescRecord *Record;

  MADB_CLEAR_ERROR(&Stmt->Error);

  if (!Stmt->metadata || Stmt->metadata->getColumnCount() == 0)
  {
    MADB_SetError(&Stmt->Error, MADB_ERR_07005, NULL, 0);
    return Stmt->Error.ReturnValue;
  }

  if (ColumnNumber < 1 || ColumnNumber > Stmt->metadata->getColumnCount())
  {
    MADB_SetError(&Stmt->Error, MADB_ERR_07009, NULL, 0);
    return SQL_ERROR;
  }
  if (!(Record= MADB_DescGetInternalRecord(Stmt->Ird, ColumnNumber - 1, MADB_DESC_WRITE)))
  {
    MADB_CopyError(&Stmt->Error, &Stmt->Ird->Error);
    return Stmt->Error.ReturnValue;
  }
  if (NameLengthPtr)
    *NameLengthPtr= 0;

  /* Don't map types if ansi mode was set */
  if (DataTypePtr)
      *DataTypePtr= (isWChar && !Stmt->Connection->IsAnsi) ? MADB_GetWCharType(Record->ConciseType) : Record->ConciseType;
  /* Columnsize in characters, not bytes! */
  if (ColumnSizePtr)
    *ColumnSizePtr= Record->Length;
     //Record->Precision ? MIN(Record->DisplaySize, Record->Precision) : Record->DisplaySize;
  if (DecimalDigitsPtr)
    *DecimalDigitsPtr= Record->Scale;
  if (NullablePtr)
    *NullablePtr= Record->Nullable;

  if ((ColumnName || BufferLength) && Record->ColumnName)
  {
    size_t Length= MADB_SetString(isWChar ? &Stmt->Connection->Charset : 0, ColumnName, ColumnName ? BufferLength : 0, Record->ColumnName, SQL_NTS, &Stmt->Error); 
    if (NameLengthPtr)
      *NameLengthPtr= (SQLSMALLINT)Length;
    if (!BufferLength)
      MADB_SetError(&Stmt->Error, MADB_ERR_01004, NULL, 0);
  }
  return Stmt->Error.ReturnValue;
}
/* }}} */

/* {{{ MADB_SetCursorName */
SQLRETURN MADB_SetCursorName(MADB_Stmt *Stmt, char *Buffer, SQLINTEGER BufferLength)
{
  MADB_List *LStmt, *LStmtNext;

  if (!Buffer)
  {
    MADB_SetError(&Stmt->Error, MADB_ERR_HY009, NULL, 0);
    return SQL_ERROR;
  }
  if (BufferLength== SQL_NTS)
    BufferLength= (SQLINTEGER)strlen(Buffer);
  if (BufferLength < 0)
  {
    MADB_SetError(&Stmt->Error, MADB_ERR_HY090, NULL, 0);
    return SQL_ERROR;
  }
  if ((BufferLength > 5 && strncmp(Buffer, "SQLCUR", 6) == 0) ||
      (BufferLength > 6 && strncmp(Buffer, "SQL_CUR", 7) == 0))
  {
    MADB_SetError(&Stmt->Error, MADB_ERR_34000, NULL, 0);
    return SQL_ERROR;
  }
  /* check if cursor name is unique */
  for (LStmt= Stmt->Connection->Stmts; LStmt; LStmt= LStmtNext)
  {
    MADB_Cursor *Cursor= &((MADB_Stmt *)LStmt->data)->Cursor;
    LStmtNext= LStmt->next;

    if (Stmt != (MADB_Stmt *)LStmt->data &&
        Cursor->Name && strncmp(Cursor->Name, Buffer, BufferLength) == 0)
    {
      MADB_SetError(&Stmt->Error, MADB_ERR_3C000, NULL, 0);
      return SQL_ERROR;
    }
  }
  MADB_FREE(Stmt->Cursor.Name);
  Stmt->Cursor.Name= static_cast<char*>(MADB_CALLOC(BufferLength + 1));
  MADB_SetString(0, Stmt->Cursor.Name, BufferLength + 1, Buffer, BufferLength, NULL);
  return SQL_SUCCESS;
}
/* }}} */

/* {{{ MADB_GetCursorName */
SQLRETURN MADB_GetCursorName(MADB_Stmt *Stmt, void *CursorName, SQLSMALLINT BufferLength, 
                             SQLSMALLINT *NameLengthPtr, my_bool isWChar)
{
  SQLSMALLINT Length;
  MADB_CLEAR_ERROR(&Stmt->Error);

  if (BufferLength < 0)
  {
    MADB_SetError(&Stmt->Error, MADB_ERR_HY090, NULL, 0);
    return Stmt->Error.ReturnValue;
  }
  if (!Stmt->Cursor.Name)
  {
    Stmt->Cursor.Name= (char *)MADB_CALLOC(MADB_MAX_CURSOR_NAME);
    _snprintf(Stmt->Cursor.Name, MADB_MAX_CURSOR_NAME, "SQL_CUR%d", 
                Stmt->Connection->CursorCount++);
  }
  Length= (SQLSMALLINT)MADB_SetString(isWChar ? &Stmt->Connection->Charset : 0, CursorName,
                                      BufferLength, Stmt->Cursor.Name, SQL_NTS, &Stmt->Error);
  if (NameLengthPtr)
    *NameLengthPtr= (SQLSMALLINT)Length;
  if (!BufferLength)
    MADB_SetError(&Stmt->Error, MADB_ERR_01004, NULL, 0);
   
  return Stmt->Error.ReturnValue;
  
}
/* }}} */

/* {{{ MADB_RefreshRowPtrs */
SQLRETURN MADB_RefreshRowPtrs(MADB_Stmt *Stmt)
{
  return SQL_SUCCESS;// MoveNext(Stmt, 1LL);
}

/* {{{ MADB_RefreshDynamicCursor */
SQLRETURN MADB_RefreshDynamicCursor(MADB_Stmt *Stmt)
{
  SQLRETURN ret;
  SQLLEN    CurrentRow=     Stmt->Cursor.Position;
  long long AffectedRows=   Stmt->AffectedRows;
  SQLLEN    LastRowFetched= Stmt->LastRowFetched;

  ret= Stmt->Methods->Execute(Stmt, FALSE);

  Stmt->Cursor.Position= CurrentRow;
  if (Stmt->Cursor.Position > 0 && (my_ulonglong)Stmt->Cursor.Position > Stmt->rs->rowsCount())
  {
    Stmt->Cursor.Position= (long)Stmt->rs->rowsCount();
  }

  Stmt->LastRowFetched= LastRowFetched;
  Stmt->AffectedRows=   AffectedRows;

  if (Stmt->Cursor.Position <= 0)
  {
    Stmt->Cursor.Position= 1;
  }
  return ret;
}
/* }}} */

/* Couple of macsros for this function specifically */
#define MADB_SETPOS_FIRSTROW(agg_result) (agg_result == SQL_INVALID_HANDLE)
#define MADB_SETPOS_AGG_RESULT(agg_result, row_result) if (MADB_SETPOS_FIRSTROW(agg_result)) agg_result= row_result; \
    else if (row_result != agg_result) agg_result= SQL_SUCCESS_WITH_INFO

/* {{{ MADB_SetPos */
SQLRETURN MADB_StmtSetPos(MADB_Stmt* Stmt, SQLSETPOSIROW RowNumber, SQLUSMALLINT Operation,
  SQLUSMALLINT LockType, int ArrayOffset)
{
  if (!Stmt->result && !Stmt->rs)
  {
    return MADB_SetError(&Stmt->Error, MADB_ERR_24000, NULL, 0);
  }

  /* This RowNumber != 1 is based on current SQL_POSITION implementation, and actually does not look to be quite correct
   */
  if (Stmt->Options.CursorType == SQL_CURSOR_FORWARD_ONLY && Operation == SQL_POSITION && RowNumber != 1)
  {
    return MADB_SetError(&Stmt->Error, MADB_ERR_HY109, NULL, 0);
  }
  /* We will break protocol, if we have any streamer. Unless this is POSITION operation - since we can have only one
     streamer at a time, we either position on cached RS, and no operation on connecion is needed, or this Stmt is
     a streamer, and we do some fetch's */
  if (Operation != SQL_POSITION && MADB_GOT_STREAMER(Stmt->Connection))
  {
    LOCK_MARIADB(Stmt->Connection);
    /* Verifying now in safe way, that we are still a streamer */
    if (MADB_GOT_STREAMER(Stmt->Connection) && Stmt->Connection->Methods->CacheRestOfCurrentRsStream(Stmt->Connection, &Stmt->Error))
    {
      UNLOCK_MARIADB(Stmt->Connection);
      return Stmt->Error.ReturnValue;
    }
    UNLOCK_MARIADB(Stmt->Connection);
  }

  if (LockType != SQL_LOCK_NO_CHANGE)
  {
    return MADB_SetError(&Stmt->Error, MADB_ERR_HYC00, NULL, 0);
  }

  switch(Operation) {
  case SQL_POSITION:
    {
      if (RowNumber < 1 || RowNumber > Stmt->rs->rowsCount())
      {
        MADB_SetError(&Stmt->Error, MADB_ERR_HY109, NULL, 0);
        return Stmt->Error.ReturnValue;
      }
      if (Stmt->Options.CursorType == SQL_CURSOR_DYNAMIC)
        if (!SQL_SUCCEEDED(Stmt->Methods->RefreshDynamicCursor(Stmt)))
          return Stmt->Error.ReturnValue;
      Stmt->Cursor.Position+= (RowNumber - 1);
      MADB_StmtDataSeek(Stmt, Stmt->Cursor.Position);
    }
    break;
  case SQL_ADD:
    {
      MADB_DynString DynStmt;
      SQLRETURN      ret;
      char          *TableName=   MADB_GetTableName(Stmt);
      char          *CatalogName= MADB_GetCatalogName(Stmt);
      int            column, param= 0;

      if (Stmt->Options.CursorType == SQL_CURSOR_DYNAMIC)
        if (!SQL_SUCCEEDED(Stmt->Methods->RefreshDynamicCursor(Stmt)))
          return Stmt->Error.ReturnValue;

      Stmt->DaeRowNumber= RowNumber;

      if (Stmt->DataExecutionType != MADB_DAE_ADD)
      {
        Stmt->Methods->StmtFree(Stmt->DaeStmt, SQL_DROP);
        MA_SQLAllocHandle(SQL_HANDLE_STMT, Stmt->Connection, (SQLHANDLE *)&Stmt->DaeStmt);

        if (MADB_InitDynamicString(&DynStmt, "INSERT INTO ", 8192, 1024) ||
            MADB_DynStrAppendQuoted(&DynStmt, CatalogName) ||
            MADB_DynstrAppend(&DynStmt, ".") ||
            MADB_DynStrAppendQuoted(&DynStmt, TableName)||
            MADB_DynStrInsertSet(Stmt, &DynStmt))
        {
          MADB_DynstrFree(&DynStmt);
          return Stmt->Error.ReturnValue;
        }

        Stmt->DaeStmt->DefaultsResult.reset(MADB_GetDefaultColumnValues(Stmt, Stmt->metadata->getFields()));

        Stmt->DataExecutionType= MADB_DAE_ADD;
        ret= Stmt->DaeStmt->Prepare(DynStmt.str, SQL_NTS, false);

        MADB_DynstrFree(&DynStmt);

        if (!SQL_SUCCEEDED(ret))
        {
          MADB_CopyError(&Stmt->Error, &Stmt->DaeStmt->Error);
          Stmt->Methods->StmtFree(Stmt->DaeStmt, SQL_DROP);
          return Stmt->Error.ReturnValue;
        }
      }
      
      /* Bind parameters - DaeStmt will process whole array of values, thus we don't need to iterate through the array*/
      for (column= 0; column < MADB_STMT_COLUMN_COUNT(Stmt); ++column)
      {
        MADB_DescRecord *Rec=          MADB_DescGetInternalRecord(Stmt->Ard, column, MADB_DESC_READ),
                        *ApdRec=       NULL;

        if (Rec->inUse && MADB_ColumnIgnoredInAllRows(Stmt->Ard, Rec) == FALSE)
        {
          Stmt->DaeStmt->Methods->BindParam(Stmt->DaeStmt, param + 1, SQL_PARAM_INPUT, Rec->ConciseType, Rec->Type,
            Rec->DisplaySize, Rec->Scale, Rec->DataPtr, Rec->OctetLength, Rec->OctetLengthPtr);
        }
        else
        {
          /*Stmt->DaeStmt->Methods->BindParam(Stmt->DaeStmt, param + 1, SQL_PARAM_INPUT, SQL_CHAR, SQL_C_CHAR, 0, 0,
                            ApdRec->DefaultValue, strlen(ApdRec->DefaultValue), NULL);*/
          continue;
        }
        
        ApdRec= MADB_DescGetInternalRecord(Stmt->DaeStmt->Apd, param, MADB_DESC_READ);
        ApdRec->DefaultValue= MADB_GetDefaultColumnValue(Stmt->DaeStmt->DefaultsResult.get(),
          Stmt->rs->getMetaData()->getFields()[column].org_name);

        ++param;
      }

      memcpy(&Stmt->DaeStmt->Apd->Header, &Stmt->Ard->Header, sizeof(MADB_Header));
      ret= Stmt->Methods->Execute(Stmt->DaeStmt, FALSE);

      if (!SQL_SUCCEEDED(ret))
      {
        /* We can have SQL_NEED_DATA here, which would not set error (and its ReturnValue) */
        MADB_CopyError(&Stmt->Error, &Stmt->DaeStmt->Error);
        return ret;
      }
      if (Stmt->AffectedRows == -1)
      {
        Stmt->AffectedRows= 0;
      }
      Stmt->AffectedRows+= Stmt->DaeStmt->AffectedRows;

      Stmt->DataExecutionType= MADB_DAE_NORMAL;
      Stmt->Methods->StmtFree(Stmt->DaeStmt, SQL_DROP);
      Stmt->DaeStmt= NULL;
    }
    break;
  case SQL_UPDATE:
    {
      char        *TableName= MADB_GetTableName(Stmt);
      my_ulonglong Start=     0, 
                   End=       Stmt->rs->rowsCount();
      SQLRETURN    result=    SQL_INVALID_HANDLE; /* Just smth we cannot normally get */   

      if (!TableName)
      {
        MADB_SetError(&Stmt->Error, MADB_ERR_IM001, "Updatable Cursors with multiple tables are not supported", 0);
        return Stmt->Error.ReturnValue;
      }
      
      Stmt->AffectedRows= 0;

      if ((SQLLEN)RowNumber > Stmt->LastRowFetched)
      {
        MADB_SetError(&Stmt->Error, MADB_ERR_S1107, NULL, 0);
        return Stmt->Error.ReturnValue;
      }

      if (RowNumber < 0 || RowNumber > End)
      {
        MADB_SetError(&Stmt->Error, MADB_ERR_HY109, NULL, 0);
        return Stmt->Error.ReturnValue;
      }

      if (Stmt->Options.CursorType == SQL_CURSOR_DYNAMIC)
        if (!SQL_SUCCEEDED(Stmt->Methods->RefreshDynamicCursor(Stmt)))
          return Stmt->Error.ReturnValue;

      Stmt->DaeRowNumber= MAX(1,RowNumber);
      
      /* Cursor is open, but no row was fetched, so we simulate
         that first row was fetched */
      if (Stmt->Cursor.Position <= 0)
        Stmt->Cursor.Position= 1;

      if (RowNumber)
        Start= End= Stmt->Cursor.Position + RowNumber - 1;
      else
      {
        Start= Stmt->Cursor.Position;
        /* TODO: if num_rows returns 1, End is 0? Start would be 1, no */
        End= MIN(Stmt->rs->rowsCount(), Start + Stmt->Ard->Header.ArraySize - 1);
      }
      /* Stmt->ArrayOffset will be incremented in StmtExecute() */
      Start+= Stmt->ArrayOffset;

      /* TODO: SQL_ATTR_ROW_STATUS_PTR should be filled */
      while (Start <= End)
      {
        SQLSMALLINT param= 0, column;
        MADB_StmtDataSeek(Stmt, Start);
        Stmt->Methods->RefreshRowPtrs(Stmt);
        
        /* We don't need to prepare the statement, if SetPos was called
           from SQLParamData() function */
        if (!ArrayOffset)
        {
          if (!SQL_SUCCEEDED(MADB_DaeStmt(Stmt, SQL_UPDATE)))
          {
            MADB_SETPOS_AGG_RESULT(result, Stmt->Error.ReturnValue);
            /* Moving to the next row */
            Stmt->DaeRowNumber++;
            Start++;

            continue;
          }

          for(column= 0; column < MADB_STMT_COLUMN_COUNT(Stmt); ++column)
          {
            SQLLEN          *LengthPtr= NULL;
            my_bool         GetDefault= FALSE;
            MADB_DescRecord *Rec=       MADB_DescGetInternalRecord(Stmt->Ard, column, MADB_DESC_READ);

            /* TODO: shouldn't here be IndicatorPtr? */
            if (Rec->OctetLengthPtr)
              LengthPtr= static_cast<SQLLEN*>(GetBindOffset(Stmt->Ard, Rec, Rec->OctetLengthPtr, Stmt->DaeRowNumber > 1 ? Stmt->DaeRowNumber - 1 : 0, sizeof(SQLLEN)));
            if (!Rec->inUse ||
                (LengthPtr && *LengthPtr == SQL_COLUMN_IGNORE))
            {
              GetDefault= TRUE;
              continue;
            }
            
            /* TODO: Looks like this whole thing is not really needed. Not quite clear if !InUse should result in going this way */
            if (GetDefault)
            {
              SQLLEN Length= 0;
              /* set a default value */
              if (Stmt->Methods->GetData(Stmt, column + 1, SQL_C_CHAR, NULL, 0, &Length, TRUE) != SQL_ERROR && Length)
              {
                MADB_FREE(Rec->DefaultValue);
                if (Length > 0) 
                {
                  Rec->DefaultValue= (char *)MADB_CALLOC(Length + 1);
                  Stmt->Methods->GetData(Stmt, column + 1, SQL_C_CHAR, Rec->DefaultValue, Length+1, 0, TRUE);
                }
                Stmt->DaeStmt->Methods->BindParam(Stmt->DaeStmt, param + 1, SQL_PARAM_INPUT, SQL_CHAR, SQL_C_CHAR, 0, 0,
                              Rec->DefaultValue, Length, NULL);
                ++param;
                continue;
              }
            }
            else
            {
              Stmt->DaeStmt->Methods->BindParam(Stmt->DaeStmt, param + 1, SQL_PARAM_INPUT, Rec->ConciseType, Rec->Type,
                      Rec->DisplaySize, Rec->Scale,
                      GetBindOffset(Stmt->Ard, Rec, Rec->DataPtr, Stmt->DaeRowNumber > 1 ? Stmt->DaeRowNumber -1 : 0, Rec->OctetLength),
                      Rec->OctetLength, LengthPtr);
            }
            if (PARAM_IS_DAE(LengthPtr) && !DAE_DONE(Stmt->DaeStmt))
            {
              Stmt->Status= SQL_NEED_DATA;
              ++param;
              continue;
            }

            ++param;
          }                             /* End of for(column=0;...) */
          if (Stmt->Status == SQL_NEED_DATA)
            return SQL_NEED_DATA;
        }                               /* End of if (!ArrayOffset) */ 
        
        if (Stmt->DaeStmt->Methods->Execute(Stmt->DaeStmt, FALSE) != SQL_ERROR)
        {
          Stmt->AffectedRows+= Stmt->DaeStmt->AffectedRows;
        }
        else
        {
          MADB_CopyError(&Stmt->Error, &Stmt->DaeStmt->Error);
        }

        MADB_SETPOS_AGG_RESULT(result, Stmt->DaeStmt->Error.ReturnValue);

        Stmt->DaeRowNumber++;
        Start++;
      }                                 /* End of while (Start <= End) */

      Stmt->Methods->StmtFree(Stmt->DaeStmt, SQL_DROP);
      Stmt->DaeStmt= NULL;
      Stmt->DataExecutionType= MADB_DAE_NORMAL;

      /* Making sure we do not return initial value */
      return result ==  SQL_INVALID_HANDLE ? SQL_SUCCESS : result;
    }
  case SQL_DELETE:
    {
      MADB_DynString DynamicStmt;
      SQLULEN        SaveArraySize= Stmt->Ard->Header.ArraySize;
      my_ulonglong   Start=         0,
                     End=           Stmt->rs->rowsCount();
      char           *TableName=    MADB_GetTableName(Stmt);

      if (!TableName)
      {
        MADB_SetError(&Stmt->Error, MADB_ERR_IM001, "Updatable Cursors with multiple tables are not supported", 0);
        return Stmt->Error.ReturnValue;
      }

      Stmt->Ard->Header.ArraySize= 1;
      if (Stmt->Options.CursorType == SQL_CURSOR_DYNAMIC)
        if (!SQL_SUCCEEDED(Stmt->Methods->RefreshDynamicCursor(Stmt)))
          return Stmt->Error.ReturnValue;
      Stmt->AffectedRows= 0;
      if (RowNumber < 0 || RowNumber > End)
      {
        MADB_SetError(&Stmt->Error, MADB_ERR_HY109, NULL, 0);
        return Stmt->Error.ReturnValue;
      }
      Start= (RowNumber) ? Stmt->Cursor.Position + RowNumber - 1 : Stmt->Cursor.Position;
      if (SaveArraySize && !RowNumber)
        End= MIN(End, Start + SaveArraySize - 1);
      else
        End= Start;

      while (Start <= End)
      {
        MADB_StmtDataSeek(Stmt, Start);
        Stmt->Methods->RefreshRowPtrs(Stmt);
        MADB_InitDynamicString(&DynamicStmt, "DELETE FROM ", 8192, 1024);
        if (MADB_DynStrAppendQuoted(&DynamicStmt, TableName) ||
           MADB_DynStrGetWhere(Stmt, &DynamicStmt, TableName, FALSE))
        {
          MADB_DynstrFree(&DynamicStmt);
          MADB_SetError(&Stmt->Error, MADB_ERR_HY001, NULL, 0);

          return Stmt->Error.ReturnValue;
        }

        LOCK_MARIADB(Stmt->Connection);
        if (mysql_real_query(Stmt->Connection->mariadb, DynamicStmt.str, (unsigned long)DynamicStmt.length))
        {
          MADB_DynstrFree(&DynamicStmt);
          MADB_SetError(&Stmt->Error, MADB_ERR_HY001, mysql_error(Stmt->Connection->mariadb), 
                            mysql_errno(Stmt->Connection->mariadb));

          UNLOCK_MARIADB(Stmt->Connection);

          return Stmt->Error.ReturnValue;
        }
        UNLOCK_MARIADB(Stmt->Connection);
        MADB_DynstrFree(&DynamicStmt);
        Stmt->AffectedRows+= mysql_affected_rows(Stmt->Connection->mariadb);
        Start++;
      }

      Stmt->Ard->Header.ArraySize= SaveArraySize;
      /* if we have a dynamic cursor we need to adjust the rowset size */
      if (Stmt->Options.CursorType == SQL_CURSOR_DYNAMIC)
      {
        Stmt->LastRowFetched-= (unsigned long)Stmt->AffectedRows;
      }
    }
    break;
  case SQL_REFRESH:
    /* todo*/
    break;
  default:
    MADB_SetError(&Stmt->Error, MADB_ERR_HYC00, "Only SQL_POSITION and SQL_REFRESH Operations are supported", 0);
    return Stmt->Error.ReturnValue;
  }
  return SQL_SUCCESS;
}
/* }}} */
#undef MADB_SETPOS_FIRSTROW
#undef MADB_SETPOS_AGG_RESULT

/* {{{ MADB_StmtFetchScroll */
SQLRETURN MADB_StmtFetchScroll(MADB_Stmt *Stmt, SQLSMALLINT FetchOrientation,
                               SQLLEN FetchOffset)
{
  SQLRETURN ret= SQL_SUCCESS;
  SQLLEN    Position= 0;
  SQLLEN    RowsProcessed= Stmt->LastRowFetched;
  
  if (!Stmt->rs)
  {
    return MADB_SetError(&Stmt->Error, MADB_ERR_24000, NULL, 0);
  }
  if (Stmt->Options.CursorType == SQL_CURSOR_FORWARD_ONLY &&
      FetchOrientation != SQL_FETCH_NEXT)
  {
    MADB_SetError(&Stmt->Error, MADB_ERR_HY106, NULL, 0);
    return Stmt->Error.ReturnValue;
  }

  if (Stmt->Options.CursorType == SQL_CURSOR_DYNAMIC)
  {
    SQLRETURN rc;
    rc= Stmt->Methods->RefreshDynamicCursor(Stmt);
    if (!SQL_SUCCEEDED(rc))
    {
      return Stmt->Error.ReturnValue;
    }
  }

  if (FetchOrientation != SQL_FETCH_NEXT)
  {
    MADB_STMT_FORGET_NEXT_POS(Stmt);
  }

  switch(FetchOrientation) {
  case SQL_FETCH_NEXT:
    Position= Stmt->Cursor.Position <= 0 ? 1 : Stmt->Cursor.Position + RowsProcessed;
    break;
  case SQL_FETCH_PRIOR:
    Position= Stmt->Cursor.Position <= 0 ? 0 : Stmt->Cursor.Position - MAX(1, Stmt->Ard->Header.ArraySize);
    break;
  case SQL_FETCH_RELATIVE:
    Position= Stmt->Cursor.Position + FetchOffset;
    if (Position <= 0 && Stmt->Cursor.Position > 1 &&
        -FetchOffset < (SQLINTEGER)Stmt->Ard->Header.ArraySize)
      Position= 1;
    break;
  case SQL_FETCH_ABSOLUTE:
    if (FetchOffset < 0)
    {
      if ((long long)Stmt->rs->rowsCount() + FetchOffset <= 0 &&
          ((SQLULEN)-FetchOffset <= Stmt->Ard->Header.ArraySize))
        Position= 1;
      else
        Position= (SQLLEN)Stmt->rs->rowsCount() + FetchOffset + 1;
    }
    else
    {
      Position = FetchOffset;
    }
    break;
  case SQL_FETCH_FIRST:
    Position= 1;
    break;
  case SQL_FETCH_LAST:
    Position= (SQLLEN)Stmt->rs->rowsCount() - MAX(0, Stmt->Ard->Header.ArraySize - 1);
 /*   if (Stmt->Ard->Header.ArraySize > 1)
      Position= MAX(0, Position - Stmt->Ard->Header.ArraySize + 1); */
    break;
  case SQL_FETCH_BOOKMARK:
    if (Stmt->Options.UseBookmarks == SQL_UB_OFF)
    {
      MADB_SetError(&Stmt->Error, MADB_ERR_HY106, NULL, 0);
      return Stmt->Error.ReturnValue;
    }
    if (!Stmt->Options.BookmarkPtr)
    {
      MADB_SetError(&Stmt->Error, MADB_ERR_HY111, NULL, 0);
      return Stmt->Error.ReturnValue;
    }

    Position= *((long *)Stmt->Options.BookmarkPtr);
    if (Stmt->Connection->Environment->OdbcVersion >= SQL_OV_ODBC3)
    {
      Position+= FetchOffset;
    }
   break;
  default:
    MADB_SetError(&Stmt->Error, MADB_ERR_HY106, NULL, 0);
    return Stmt->Error.ReturnValue;
    break;
  }

  if (Position <= 0)
  {
    MADB_STMT_RESET_CURSOR(Stmt);
  }
  else
  {
    Stmt->Cursor.Position= (SQLLEN)MIN((my_ulonglong)Position, Stmt->rs->rowsCount() + 1);
  }
  if (Position <= 0 || !MADB_STMT_SHOULD_STREAM(Stmt) && (my_ulonglong)Position > Stmt->rs->rowsCount())
  {
    /* We need to put cursor before RS start, not only return error */
    if (Position <= 0)
    {
      MADB_StmtDataSeek(Stmt, 0);
    }
    return SQL_NO_DATA;
  }

  /* For dynamic cursor we "refresh" resultset each time(basically re-executing), and thus the (c/c)cursor is before 1st row at this point,
     and thus we need to restore the last position. For array fetch with not forward_only cursor, the (c/c)cursor is at 1st row of the last
     fetched rowset */
  if (FetchOrientation != SQL_FETCH_NEXT || (RowsProcessed > 1 && Stmt->Options.CursorType != SQL_CURSOR_FORWARD_ONLY) ||
      Stmt->Options.CursorType == SQL_CURSOR_DYNAMIC)
  {
    if (Stmt->Cursor.Next != -1)
    {
      Stmt->rs->absolute(Stmt->Cursor.Next);
      ret= SQL_SUCCESS;
    }
    else
    {
      // We need - 1 since Fetch calls next. And Fetch calls next since it has in case of array of rows fetching
      ret= MADB_StmtDataSeek(Stmt, Stmt->Cursor.Position - 1);
    }
  }
  
  /* Assuming, that ret before previous "if" was SQL_SUCCESS */
  if (ret == SQL_SUCCESS)
  {
    ret= Stmt->Methods->Fetch(Stmt);
  }
  if (ret == SQL_NO_DATA_FOUND && Stmt->LastRowFetched > 0)
  {
    ret= SQL_SUCCESS;
  }
  return ret;
}

struct st_ma_stmt_methods MADB_StmtMethods=
{
  MADB_StmtExecute,
  MADB_StmtFetch,
  MADB_StmtBindCol,
  MADB_StmtBindParam,
  MADB_StmtExecDirect,
  MADB_StmtGetData,
  MADB_StmtRowCount,
  MADB_StmtParamCount,
  MADB_StmtColumnCount,
  MADB_StmtGetAttr,
  MADB_StmtSetAttr,
  MADB_StmtFree,
  MADB_StmtColAttr,
  MADB_StmtColumnPrivileges,
  MADB_StmtTablePrivileges,
  MADB_StmtTables,
  MADB_StmtStatistics,
  MADB_StmtColumns,
  MADB_StmtProcedureColumns,
  MADB_StmtPrimaryKeys,
  MADB_StmtSpecialColumns,
  MADB_StmtProcedures,
  MADB_StmtForeignKeys,
  MADB_StmtDescribeCol,
  MADB_SetCursorName,
  MADB_GetCursorName,
  MADB_StmtSetPos,
  MADB_StmtFetchScroll,
  MADB_StmtParamData,
  MADB_StmtPutData,
  MADB_StmtBulkOperations,
  MADB_RefreshDynamicCursor,
  MADB_RefreshRowPtrs
};

MADB_Stmt::MADB_Stmt(MADB_Dbc * Dbc)
  : Connection(Dbc),
  DefaultsResult(nullptr, &mysql_free_result)
{
  std::memset(&Error, 0, sizeof(MADB_Error));
  std::memset(&Bulk, 0, sizeof(MADB_BulkOperationInfo));
  std::memset(&Options, 0, sizeof(MADB_StmtOptions));
  std::memset(&Cursor, 0, sizeof(MADB_Cursor));
  std::memset(&ListItem, 0, sizeof(MADB_List));
}

/* {{{ MADB_StmtInit */
SQLRETURN MADB_StmtInit(MADB_Dbc *Connection, SQLHANDLE *pHStmt)
{
  MADB_Stmt *Stmt= new MADB_Stmt(Connection);
 
  MADB_PutErrorPrefix(Connection, &Stmt->Error);
  *pHStmt= Stmt;
  Stmt->Connection= Connection;
 
  LOCK_MARIADB(Connection);
  Stmt->stmt.reset();
  if (!(Stmt->IApd= MADB_DescInit(Connection, MADB_DESC_APD, FALSE)) ||
    !(Stmt->IArd= MADB_DescInit(Connection, MADB_DESC_ARD, FALSE)) ||
    !(Stmt->IIpd= MADB_DescInit(Connection, MADB_DESC_IPD, FALSE)) ||
    !(Stmt->IIrd= MADB_DescInit(Connection, MADB_DESC_IRD, FALSE)))
  {
    UNLOCK_MARIADB(Stmt->Connection);
    goto error;
  }

  MDBUG_C_PRINT(Stmt->Connection, "-->inited %0x", Stmt->stmt.get());
  UNLOCK_MARIADB(Connection);

  Stmt->Methods= &MADB_StmtMethods;

  Stmt->Options.CursorType= SQL_CURSOR_FORWARD_ONLY;

  Stmt->Options.UseBookmarks= SQL_UB_OFF;
  Stmt->Options.MetadataId= Connection->MetadataId;

  Stmt->Apd= Stmt->IApd;
  Stmt->Ard= Stmt->IArd;
  Stmt->Ipd= Stmt->IIpd;
  Stmt->Ird= Stmt->IIrd;
  
  Stmt->ListItem.data= (void *)Stmt;
  EnterCriticalSection(&Stmt->Connection->ListsCs);
  Stmt->Connection->Stmts= MADB_ListAdd(Stmt->Connection->Stmts, &Stmt->ListItem);
  LeaveCriticalSection(&Stmt->Connection->ListsCs);

  Stmt->Ard->Header.ArraySize= 1;

  return SQL_SUCCESS;

error:
  if (Stmt && Stmt->stmt)
  {
    MADB_STMT_CLOSE_STMT(Stmt);
  }
  MADB_DescFree(Stmt->IApd, TRUE);
  MADB_DescFree(Stmt->IArd, TRUE);
  MADB_DescFree(Stmt->IIpd, TRUE);
  MADB_DescFree(Stmt->IIrd, TRUE);

  delete Stmt;

  return SQL_ERROR;
}
/* }}} */
