/*
 * Copyright (C) 2010 SCALITY SA. All rights reserved.
 * http://www.scality.com
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 * Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY SCALITY SA ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL SCALITY SA OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * The views and conclusions contained in the software and documentation
 * are those of the authors and should not be interpreted as representing
 * official policies, either expressed or implied, of SCALITY SA.
 *
 * https://github.com/scality/Droplet
 */
#include "dropletp.h"
#include <droplet/cdmi/reqbuilder.h>

//#define DPRINTF(fmt,...) fprintf(stderr, fmt, ##__VA_ARGS__)
#define DPRINTF(fmt,...)

dpl_status_t
dpl_cdmi_make_bucket(dpl_ctx_t *ctx,
                     char *bucket,
                     dpl_location_constraint_t location_constraint,
                     dpl_canned_acl_t canned_acl)
{
  return DPL_SUCCESS;
}

/**
 * list bucket
 *
 * @param ctx
 * @param bucket
 * @param prefix can be NULL
 * @param delimiter can be NULL
 * @param objectsp
 * @param prefixesp
 *
 * @return
 */
dpl_status_t
dpl_cdmi_list_bucket(dpl_ctx_t *ctx,
                     char *bucket,
                     char *prefix,
                     char *delimiter,
                     dpl_vec_t **objectsp,
                     dpl_vec_t **common_prefixesp)
{
  char          *host;
  int           ret, ret2;
  dpl_conn_t   *conn = NULL;
  char          header[1024];
  u_int         header_len;
  struct iovec  iov[10];
  int           n_iov = 0;
  int           connection_close = 0;
  char          *data_buf = NULL;
  u_int         data_len;
  dpl_dict_t    *headers_request = NULL;
  dpl_dict_t    *headers_reply = NULL;
  dpl_dict_t    *metadata = NULL;
  dpl_req_t     *req = NULL;
  dpl_vec_t     *common_prefixes = NULL;
  dpl_vec_t     *objects = NULL;

  DPL_TRACE(ctx, DPL_TRACE_CONV, "list bucket=%s prefix=%s", bucket, prefix);

  req = dpl_req_new(ctx);
  if (NULL == req)
    {
      ret = DPL_ENOMEM;
      goto end;
    }

  dpl_req_set_method(req, DPL_METHOD_GET);

  ret2 = dpl_req_set_resource(req, NULL != prefix ? prefix : "/");
  if (DPL_SUCCESS != ret2)
    {
      ret = ret2;
      goto end;
    }

  //contact default host
  dpl_req_rm_behavior(req, DPL_BEHAVIOR_VIRTUAL_HOSTING);

  dpl_req_set_object_type(req, DPL_OBJECT_TYPE_CONTAINER);

  //build request
  ret2 = dpl_cdmi_req_build(req, &headers_request, NULL, NULL);
  if (DPL_SUCCESS != ret2)
    {
      ret = DPL_FAILURE;
      goto end;
    }

  host = dpl_dict_get_value(headers_request, "Host");
  if (NULL == host)
    {
      ret = DPL_FAILURE;
      goto end;
    }

  conn = dpl_conn_open_host(ctx, host, ctx->port);
  if (NULL == conn)
    {
      ret = DPL_FAILURE;
      goto end;
    }

  //bucket emulation
  ret2 = dpl_dict_add(headers_request, "X-Scality-Bucket", bucket, 0);
  if (DPL_SUCCESS != ret2)
    {
      ret = ret2;
      goto end;
    }

  ret2 = dpl_req_gen_http_request(ctx, req, headers_request, NULL, header, sizeof (header), &header_len);
  if (DPL_SUCCESS != ret2)
    {
      ret = DPL_FAILURE;
      goto end;
    }

  iov[n_iov].iov_base = header;
  iov[n_iov].iov_len = header_len;
  n_iov++;

  //final crlf
  iov[n_iov].iov_base = "\r\n";
  iov[n_iov].iov_len = 2;
  n_iov++;

  ret2 = dpl_conn_writev_all(conn, iov, n_iov, conn->ctx->write_timeout);
  if (DPL_SUCCESS != ret2)
    {
      DPLERR(1, "writev failed");
      connection_close = 1;
      ret = DPL_ENOENT; //mapped to 404
      goto end;
    }

  ret2 = dpl_read_http_reply(conn, 1, &data_buf, &data_len, &headers_reply);
  if (DPL_SUCCESS != ret2)
    {
      if (DPL_ENOENT == ret2)
        {
          ret = DPL_ENOENT;
          goto end;
        }
      else
        {
          DPLERR(0, "read http answer failed");
          connection_close = 1;
          ret = DPL_ENOENT; //mapped to 404
          goto end;
        }
    }
  else
    {
      connection_close = dpl_connection_close(headers_reply);
    }

  (void) dpl_log_charged_event(ctx, "DATA", "OUT", data_len);

  objects = dpl_vec_new(2, 2);
  if (NULL == objects)
    {
      ret = DPL_FAILURE;
      goto end;
    }

  common_prefixes = dpl_vec_new(2, 2);
  if (NULL == common_prefixes)
    {
      ret = DPL_FAILURE;
      goto end;
    }

  ret = dpl_cdmi_parse_list_bucket(ctx, data_buf, data_len, prefix, objects, common_prefixes);
  if (DPL_SUCCESS != ret)
    {
      ret = DPL_FAILURE;
      goto end;
    }

  if (NULL != objectsp)
    {
      *objectsp = objects;
      objects = NULL; //consume it
    }

  if (NULL != common_prefixesp)
    {
      *common_prefixesp = common_prefixes;
      common_prefixes = NULL; //consume it
    }

  ret = DPL_SUCCESS;

 end:

  if (NULL != objects)
    dpl_vec_objects_free(objects);

  if (NULL != common_prefixes)
    dpl_vec_common_prefixes_free(common_prefixes);

  if (NULL != data_buf)
    free(data_buf);

  if (NULL != conn)
    {
      if (1 == connection_close)
        dpl_conn_terminate(conn);
      else
        dpl_conn_release(conn);
    }

  if (NULL != headers_reply)
    dpl_dict_free(headers_reply);

  if (NULL != headers_request)
    dpl_dict_free(headers_request);

  if (NULL != req)
    dpl_req_free(req);

  DPRINTF("ret=%d\n", ret);

  return ret;
}

/**
 * put a resource
 *
 * @param ctx
 * @param bucket
 * @param resource
 * @param subresource can be NULL
 * @param metadata can be NULL
 * @param canned_acl
 * @param data_buf
 * @param data_len
 *
 * @return
 */
dpl_status_t
dpl_cdmi_put(dpl_ctx_t *ctx,
             char *bucket,
             char *resource,
             char *subresource,
             dpl_object_type_t object_type,
             dpl_dict_t *metadata,
             dpl_canned_acl_t canned_acl,
             char *data_buf,
             unsigned int data_len)
{
  char          *host;
  int           ret, ret2;
  dpl_conn_t   *conn = NULL;
  char          header[1024];
  u_int         header_len;
  struct iovec  iov[10];
  int           n_iov = 0;
  int           connection_close = 0;
  dpl_dict_t    *headers_request = NULL;
  dpl_dict_t    *headers_reply = NULL;
  dpl_req_t     *req = NULL;
  dpl_chunk_t   chunk;
  char          *body_str = NULL;
  int           body_len = 0;

  DPL_TRACE(ctx, DPL_TRACE_CONV, "get bucket=%s resource=%s", bucket, resource);

  req = dpl_req_new(ctx);
  if (NULL == req)
    {
      ret = DPL_ENOMEM;
      goto end;
    }

  dpl_req_set_method(req, DPL_METHOD_PUT);

  ret2 = dpl_req_set_resource(req, resource);
  if (DPL_SUCCESS != ret2)
    {
      ret = ret2;
      goto end;
    }

  if (NULL != subresource)
    {
      ret2 = dpl_req_set_subresource(req, subresource);
      if (DPL_SUCCESS != ret2)
        {
          ret = ret2;
          goto end;
        }
    }

  //contact default host
  dpl_req_rm_behavior(req, DPL_BEHAVIOR_VIRTUAL_HOSTING);

  dpl_req_set_object_type(req, object_type);

  chunk.buf = data_buf;
  chunk.len = data_len;
  dpl_req_set_chunk(req, &chunk);

  dpl_req_add_behavior(req, DPL_BEHAVIOR_MD5);

  dpl_req_set_canned_acl(req, canned_acl);

  if (NULL != metadata)
    {
      ret2 = dpl_req_add_metadata(req, metadata);
      if (DPL_SUCCESS != ret2)
        {
          ret = ret2;
          goto end;
        }
    }

  //build request
  ret2 = dpl_cdmi_req_build(req, &headers_request, &body_str, &body_len);
  if (DPL_SUCCESS != ret2)
    {
      ret = DPL_FAILURE;
      goto end;
    }

  host = dpl_dict_get_value(headers_request, "Host");
  if (NULL == host)
    {
      ret = DPL_FAILURE;
      goto end;
    }

  conn = dpl_conn_open_host(ctx, host, ctx->port);
  if (NULL == conn)
    {
      ret = DPL_FAILURE;
      goto end;
    }

  //bucket emulation
  ret2 = dpl_dict_add(headers_request, "X-Scality-Bucket", bucket, 0);
  if (DPL_SUCCESS != ret2)
    {
      ret = ret2;
      goto end;
    }

  ret2 = dpl_req_gen_http_request(ctx, req, headers_request, NULL, header, sizeof (header), &header_len);
  if (DPL_SUCCESS != ret2)
    {
      ret = DPL_FAILURE;
      goto end;
    }

  iov[n_iov].iov_base = header;
  iov[n_iov].iov_len = header_len;
  n_iov++;

  //final crlf
  iov[n_iov].iov_base = "\r\n";
  iov[n_iov].iov_len = 2;
  n_iov++;

  //buffer
  iov[n_iov].iov_base = body_str;
  iov[n_iov].iov_len = body_len;
  n_iov++;

  ret2 = dpl_conn_writev_all(conn, iov, n_iov, conn->ctx->write_timeout);
  if (DPL_SUCCESS != ret2)
    {
      DPLERR(1, "writev failed");
      connection_close = 1;
      ret = DPL_ENOENT; //mapped to 404
      goto end;
    }

  ret2 = dpl_read_http_reply(conn, 1, NULL, NULL, &headers_reply);
  if (DPL_SUCCESS != ret2)
    {
      if (DPL_ENOENT == ret2)
        {
          ret = DPL_ENOENT;
          goto end;
        }
      else
        {
          DPLERR(0, "read http answer failed");
          connection_close = 1;
          ret = DPL_ENOENT; //mapped to 404
          goto end;
        }
    }
  else
    {
      connection_close = dpl_connection_close(headers_reply);
    }

  (void) dpl_log_charged_event(ctx, "DATA", "IN", data_len);

  ret = DPL_SUCCESS;

 end:

  if (NULL != body_str)
    free(body_str);

  if (NULL != conn)
    {
      if (1 == connection_close)
        dpl_conn_terminate(conn);
      else
        dpl_conn_release(conn);
    }

  if (NULL != headers_reply)
    dpl_dict_free(headers_reply);

  if (NULL != headers_request)
    dpl_dict_free(headers_request);

  if (NULL != req)
    dpl_req_free(req);

  DPRINTF("ret=%d\n", ret);

  return ret;
}

dpl_status_t
dpl_cdmi_put_buffered(dpl_ctx_t *ctx,
                      char *bucket,
                      char *resource,
                      char *subresource,
                      dpl_object_type_t object_type,
                      dpl_dict_t *metadata,
                      dpl_canned_acl_t canned_acl,
                      unsigned int data_len,
                      dpl_conn_t **connp)
{
  char          *host;
  int           ret, ret2;
  dpl_conn_t    *conn = NULL;
  char          header[1024];
  u_int         header_len;
  struct iovec  iov[10];
  int           n_iov = 0;
  int           connection_close = 0;
  dpl_dict_t    *headers_request = NULL;
  dpl_dict_t    *headers_reply = NULL;
  dpl_req_t     *req = NULL;
  dpl_chunk_t   chunk;

  DPL_TRACE(ctx, DPL_TRACE_CONV, "put_buffered bucket=%s resource=%s", bucket, resource);

  req = dpl_req_new(ctx);
  if (NULL == req)
    {
      ret = DPL_ENOMEM;
      goto end;
    }

  dpl_req_set_method(req, DPL_METHOD_PUT);

  ret2 = dpl_req_set_bucket(req, bucket);
  if (DPL_SUCCESS != ret2)
    {
      ret = ret2;
      goto end;
    }

  ret2 = dpl_req_set_resource(req, resource);
  if (DPL_SUCCESS != ret2)
    {
      ret = ret2;
      goto end;
    }

  if (NULL != subresource)
    {
      ret2 = dpl_req_set_subresource(req, subresource);
      if (DPL_SUCCESS != ret2)
        {
          ret = ret2;
          goto end;
        }
    }

  chunk.buf = NULL;
  chunk.len = data_len;
  dpl_req_set_chunk(req, &chunk);

  //contact default host
  dpl_req_rm_behavior(req, DPL_BEHAVIOR_VIRTUAL_HOSTING);

  dpl_req_add_behavior(req, DPL_BEHAVIOR_HTTP_COMPAT);

  dpl_req_set_object_type(req, object_type);

  dpl_req_add_behavior(req, DPL_BEHAVIOR_EXPECT);

  dpl_req_set_canned_acl(req, canned_acl);

  if (NULL != metadata)
    {
      ret2 = dpl_req_add_metadata(req, metadata);
      if (DPL_SUCCESS != ret2)
        {
          ret = ret2;
          goto end;
        }
    }

  ret2 = dpl_cdmi_req_build(req, &headers_request, NULL, NULL);
  if (DPL_SUCCESS != ret2)
    {
      ret = DPL_FAILURE;
      goto end;
    }

  host = dpl_dict_get_value(headers_request, "Host");
  if (NULL == host)
    {
      ret = DPL_FAILURE;
      goto end;
    }

  conn = dpl_conn_open_host(ctx, host, ctx->port);
  if (NULL == conn)
    {
      ret = DPL_FAILURE;
      goto end;
    }

  //bucket emulation
  ret2 = dpl_dict_add(headers_request, "X-Scality-Bucket", bucket, 0);
  if (DPL_SUCCESS != ret2)
    {
      ret = ret2;
      goto end;
    }

  ret2 = dpl_req_gen_http_request(ctx, req, headers_request, NULL, header, sizeof (header), &header_len);
  if (DPL_SUCCESS != ret2)
    {
      ret = DPL_FAILURE;
      goto end;
    }

  iov[n_iov].iov_base = header;
  iov[n_iov].iov_len = header_len;
  n_iov++;

  //final crlf
  iov[n_iov].iov_base = "\r\n";
  iov[n_iov].iov_len = 2;
  n_iov++;

  ret2 = dpl_conn_writev_all(conn, iov, n_iov, conn->ctx->write_timeout);
  if (DPL_SUCCESS != ret2)
    {
      DPLERR(1, "writev failed");
      connection_close = 1;
      ret = DPL_ENOENT; //mapped to 404
      goto end;
    }

  ret2 = dpl_read_http_reply(conn, 1, NULL, NULL, &headers_reply);
  if (DPL_SUCCESS != ret2)
    {
      if (DPL_ENOENT == ret2)
        {
          ret = DPL_ENOENT;
          goto end;
        }
      else
        {
          DPLERR(0, "read http answer failed");
          connection_close = 1;
          ret = DPL_ENOENT; //mapped to 404
          goto end;
        }
    }
  else
    {
      if (NULL != headers_reply) //possible if continue succeeded
        connection_close = dpl_connection_close(headers_reply);
    }

  (void) dpl_log_charged_event(ctx, "DATA", "IN", data_len);

  if (NULL != connp)
    {
      *connp = conn;
      conn = NULL; //consume it
    }

  ret = DPL_SUCCESS;

 end:

  if (NULL != conn)
    {
      if (1 == connection_close)
        dpl_conn_terminate(conn);
      else
        dpl_conn_release(conn);
    }

  if (NULL != headers_reply)
    dpl_dict_free(headers_reply);

  if (NULL != headers_request)
    dpl_dict_free(headers_request);

  if (NULL != req)
    dpl_req_free(req);

  DPRINTF("ret=%d\n", ret);

  return ret;
}

/**
 * get a resource
 *
 * @param ctx
 * @param bucket
 * @param resource
 * @param subresource can be NULL
 * @param condition can be NULL
 * @param data_bufp must be freed by caller
 * @param data_lenp
 * @param metadatap must be freed by caller
 *
 * @return
 */
dpl_status_t
dpl_cdmi_get(dpl_ctx_t *ctx,
           char *bucket,
           char *resource,
           char *subresource,
           dpl_condition_t *condition,
           char **data_bufp,
           unsigned int *data_lenp,
           dpl_dict_t **metadatap)
{
  return DPL_ENOTSUPP;
}

/**
 * get a resource
 *
 * @param ctx
 * @param bucket
 * @param resource
 * @param subresource can be NULL
 * @param condition can be NULL
 * @param data_bufp must be freed by caller
 * @param data_lenp
 * @param metadatap must be freed by caller
 *
 * @return
 */
dpl_status_t
dpl_cdmi_get_range(dpl_ctx_t *ctx,
                 char *bucket,
                 char *resource,
                 char *subresource,
                 dpl_condition_t *condition,
                 int start,
                 int end,
                 char **data_bufp,
                 unsigned int *data_lenp,
                 dpl_dict_t **metadatap)
{
  return DPL_ENOTSUPP;
}

struct get_conven
{
  dpl_header_func_t header_func;
  dpl_buffer_func_t buffer_func;
  void *cb_arg;
  int connection_close;
};

static dpl_status_t
cb_get_header(void *cb_arg,
              char *header,
              char *value)
{
  struct get_conven *gc = (struct get_conven *) cb_arg;
  int ret;

  if (NULL != gc->header_func)
    {
      ret = gc->header_func(gc->cb_arg, header, value);
      if (DPL_SUCCESS != ret)
        return ret;
    }

  if (!strcasecmp(header, "connection"))
    {
      if (!strcasecmp(value, "close"))
        gc->connection_close = 1;
    }

  return DPL_SUCCESS;
}

static dpl_status_t
cb_get_buffer(void *cb_arg,
              char *buf,
              unsigned int len)
{
  struct get_conven *gc = (struct get_conven *) cb_arg;
  int ret;

  if (NULL != gc->buffer_func)
    {
      ret = gc->buffer_func(gc->cb_arg, buf, len);
      if (DPL_SUCCESS != ret)
        return ret;
    }

  return DPL_SUCCESS;
}

dpl_status_t
dpl_cdmi_get_buffered(dpl_ctx_t *ctx,
                    char *bucket,
                    char *resource,
                    char *subresource,
                    dpl_condition_t *condition,
                    dpl_header_func_t header_func,
                    dpl_buffer_func_t buffer_func,
                    void *cb_arg)
{
  char          *host;
  int           ret, ret2;
  dpl_conn_t   *conn = NULL;
  char          header[1024];
  u_int         header_len;
  struct iovec  iov[10];
  int           n_iov = 0;
  dpl_dict_t    *headers_request = NULL;
  dpl_req_t     *req = NULL;
  struct get_conven gc;

  DPL_TRACE(ctx, DPL_TRACE_CONV, "get_buffered bucket=%s resource=%s", bucket, resource);

  memset(&gc, 0, sizeof (gc));
  gc.header_func = header_func;
  gc.buffer_func = buffer_func;
  gc.cb_arg = cb_arg;

  req = dpl_req_new(ctx);
  if (NULL == req)
    {
      ret = DPL_ENOMEM;
      goto end;
    }

  dpl_req_set_method(req, DPL_METHOD_GET);

  ret2 = dpl_req_set_bucket(req, bucket);
  if (DPL_SUCCESS != ret2)
    {
      ret = ret2;
      goto end;
    }

  ret2 = dpl_req_set_resource(req, resource);
  if (DPL_SUCCESS != ret2)
    {
      ret = ret2;
      goto end;
    }

  if (NULL != subresource)
    {
      ret2 = dpl_req_set_subresource(req, subresource);
      if (DPL_SUCCESS != ret2)
        {
          ret = ret2;
          goto end;
        }
    }

  if (NULL != condition)
    {
      dpl_req_set_condition(req, condition);
    }

  //contact default host
  dpl_req_rm_behavior(req, DPL_BEHAVIOR_VIRTUAL_HOSTING);

  dpl_req_add_behavior(req, DPL_BEHAVIOR_HTTP_COMPAT);

  //build request
  ret2 = dpl_cdmi_req_build(req, &headers_request, NULL, NULL);
  if (DPL_SUCCESS != ret2)
    {
      ret = DPL_FAILURE;
      goto end;
    }

  host = dpl_dict_get_value(headers_request, "Host");
  if (NULL == host)
    {
      ret = DPL_FAILURE;
      goto end;
    }

  conn = dpl_conn_open_host(ctx, host, ctx->port);
  if (NULL == conn)
    {
      ret = DPL_FAILURE;
      goto end;
    }

  //bucket emulation
  ret2 = dpl_dict_add(headers_request, "X-Scality-Bucket", bucket, 0);
  if (DPL_SUCCESS != ret2)
    {
      ret = ret2;
      goto end;
    }

  ret2 = dpl_req_gen_http_request(ctx, req, headers_request, NULL, header, sizeof (header), &header_len);
  if (DPL_SUCCESS != ret2)
    {
      ret = DPL_FAILURE;
      goto end;
    }

  iov[n_iov].iov_base = header;
  iov[n_iov].iov_len = header_len;
  n_iov++;

  //final crlf
  iov[n_iov].iov_base = "\r\n";
  iov[n_iov].iov_len = 2;
  n_iov++;

  ret2 = dpl_conn_writev_all(conn, iov, n_iov, conn->ctx->write_timeout);
  if (DPL_SUCCESS != ret2)
    {
      DPLERR(1, "writev failed");
      gc.connection_close = 1;
      ret = DPL_ENOENT; //mapped to 404
      goto end;
    }

  ret2 = dpl_read_http_reply_buffered(conn, 1, cb_get_header, cb_get_buffer, &gc);
  if (DPL_SUCCESS != ret2)
    {
      if (DPL_ENOENT == ret2)
        {
          ret = DPL_ENOENT;
          goto end;
        }
      else
        {
          DPLERR(0, "read http answer failed");
          gc.connection_close = 1;
          ret = DPL_ENOENT; //mapped to 404
          goto end;
        }
    }

  //caller is responsible for logging the event

  ret = DPL_SUCCESS;

 end:

  if (NULL != conn)
    {
      if (1 == gc.connection_close)
        dpl_conn_terminate(conn);
      else
        dpl_conn_release(conn);
    }

  if (NULL != headers_request)
    dpl_dict_free(headers_request);

  if (NULL != req)
    dpl_req_free(req);

  DPRINTF("ret=%d\n", ret);

  return ret;
}


dpl_status_t
dpl_cdmi_head(dpl_ctx_t *ctx,
            char *bucket,
            char *resource,
            char *subresource,
            dpl_condition_t *condition,
            dpl_dict_t **metadatap)
{
  return DPL_ENOTSUPP;
}

dpl_status_t
dpl_cdmi_head_all(dpl_ctx_t *ctx,
                char *bucket,
                char *resource,
                char *subresource,
                dpl_condition_t *condition,
                dpl_dict_t **metadatap)
{
  return DPL_ENOTSUPP;
}


/**
 * delete a resource
 *
 * @param ctx
 * @param bucket
 * @param resource
 * @param subresource can be NULL
 *
 * @return
 */
dpl_status_t
dpl_cdmi_delete(dpl_ctx_t *ctx,
              char *bucket,
              char *resource,
              char *subresource)
{
  char          *host;
  int           ret, ret2;
  dpl_conn_t   *conn = NULL;
  char          header[1024];
  u_int         header_len;
  struct iovec  iov[10];
  int           n_iov = 0;
  int           connection_close = 0;
  dpl_dict_t    *headers_request = NULL;
  dpl_dict_t    *headers_reply = NULL;
  dpl_req_t     *req = NULL;
  dpl_chunk_t   chunk;

  DPL_TRACE(ctx, DPL_TRACE_CONV, "get bucket=%s resource=%s", bucket, resource);

  req = dpl_req_new(ctx);
  if (NULL == req)
    {
      ret = DPL_ENOMEM;
      goto end;
    }

  dpl_req_set_method(req, DPL_METHOD_DELETE);

  ret2 = dpl_req_set_resource(req, resource);
  if (DPL_SUCCESS != ret2)
    {
      ret = ret2;
      goto end;
    }

  if (NULL != subresource)
    {
      ret2 = dpl_req_set_subresource(req, subresource);
      if (DPL_SUCCESS != ret2)
        {
          ret = ret2;
          goto end;
        }
    }

  //contact default host
  dpl_req_rm_behavior(req, DPL_BEHAVIOR_VIRTUAL_HOSTING);

  dpl_req_add_behavior(req, DPL_BEHAVIOR_HTTP_COMPAT);

  //build request
  ret2 = dpl_cdmi_req_build(req, &headers_request, NULL, NULL);
  if (DPL_SUCCESS != ret2)
    {
      ret = DPL_FAILURE;
      goto end;
    }

  host = dpl_dict_get_value(headers_request, "Host");
  if (NULL == host)
    {
      ret = DPL_FAILURE;
      goto end;
    }

  conn = dpl_conn_open_host(ctx, host, ctx->port);
  if (NULL == conn)
    {
      ret = DPL_FAILURE;
      goto end;
    }

  //bucket emulation
  ret2 = dpl_dict_add(headers_request, "X-Scality-Bucket", bucket, 0);
  if (DPL_SUCCESS != ret2)
    {
      ret = ret2;
      goto end;
    }

  ret2 = dpl_req_gen_http_request(ctx, req, headers_request, NULL, header, sizeof (header), &header_len);
  if (DPL_SUCCESS != ret2)
    {
      ret = DPL_FAILURE;
      goto end;
    }

  iov[n_iov].iov_base = header;
  iov[n_iov].iov_len = header_len;
  n_iov++;

  //final crlf
  iov[n_iov].iov_base = "\r\n";
  iov[n_iov].iov_len = 2;
  n_iov++;

  ret2 = dpl_conn_writev_all(conn, iov, n_iov, conn->ctx->write_timeout);
  if (DPL_SUCCESS != ret2)
    {
      DPLERR(1, "writev failed");
      connection_close = 1;
      ret = DPL_ENOENT; //mapped to 404
      goto end;
    }

  ret2 = dpl_read_http_reply(conn, 1, NULL, NULL, &headers_reply);
  if (DPL_SUCCESS != ret2)
    {
      if (DPL_ENOENT == ret2)
        {
          ret = DPL_ENOENT;
          goto end;
        }
      else
        {
          DPLERR(0, "read http answer failed");
          connection_close = 1;
          ret = DPL_ENOENT; //mapped to 404
          goto end;
        }
    }
  else
    {
      connection_close = dpl_connection_close(headers_reply);
    }

  (void) dpl_log_charged_event(ctx, "REQUEST", "DELETE", 0);

  ret = DPL_SUCCESS;

 end:

  if (NULL != conn)
    {
      if (1 == connection_close)
        dpl_conn_terminate(conn);
      else
        dpl_conn_release(conn);
    }

  if (NULL != headers_reply)
    dpl_dict_free(headers_reply);

  if (NULL != headers_request)
    dpl_dict_free(headers_request);

  if (NULL != req)
    dpl_req_free(req);

  DPRINTF("ret=%d\n", ret);

  return ret;
}
