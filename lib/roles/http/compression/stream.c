/*
 * libwebsockets - small server side websockets and web server implementation
 *
 * Copyright (C) 2010 - 2018 Andy Green <andy@warmcat.com>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation:
 *  version 2.1 of the License.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 *  MA  02110-1301  USA
 */

#include "core/private.h"

/* compression methods listed in order of preference */

struct lws_compression_support *lcs_available[] = {
#if defined(LWS_WITH_HTTP_BROTLI)
	&lcs_brotli,
#endif
	&lcs_deflate,
};

LWS_VISIBLE int
lws_http_compression_apply(struct lws *wsi, const char *name,
			   unsigned char **p, unsigned char *end, char decomp)
{
	size_t n;
	const char *a = lws_hdr_simple_ptr(wsi, WSI_TOKEN_HTTP_ACCEPT_ENCODING);

	for (n = 0; n < LWS_ARRAY_SIZE(lcs_available); n++) {
		/* if name is non-NULL, choose only that compression method */
		if (name && !strcmp(lcs_available[n]->encoding_name, name))
			continue;
		/*
		 * If we're the server, confirm that the client told us he could
		 * handle this kind of compression transform...
		 */
		if (!decomp && lwsi_role_server(wsi) &&
		    (!a || !strstr(a, lcs_available[n]->encoding_name)))
			continue;

		/* let's go with this one then... */
		break;
	}

	if (n == LWS_ARRAY_SIZE(lcs_available))
		return 1;

	lcs_available[n]->init_compression(&wsi->http.comp_ctx, decomp);
	if (!wsi->http.comp_ctx.u.generic_ctx_ptr)
		return 1;

	wsi->http.lcs = lcs_available[n];
	wsi->http.comp_ctx.may_have_more = 0;
	wsi->http.comp_ctx.final_on_input_side = 0;
	wsi->http.comp_ctx.chunking = 0;
	wsi->http.comp_ctx.is_decompression = decomp;

	if (lws_add_http_header_by_token(wsi, WSI_TOKEN_HTTP_CONTENT_ENCODING,
			(unsigned char *)lcs_available[n]->encoding_name,
			strlen(lcs_available[n]->encoding_name), p, end))
		return -1;

	lwsl_info("%s: wsi %p: applied %s content-encoding\n", __func__,
		    wsi, lcs_available[n]->encoding_name);

	return 0;
}

void
lws_http_compression_destroy(struct lws *wsi)
{
	if (!wsi->http.lcs || !wsi->http.comp_ctx.u.generic_ctx_ptr)
		return;

	wsi->http.lcs->destroy(&wsi->http.comp_ctx);

	wsi->http.lcs = NULL;
}

/*
 * This manages the compression transform independent of h1 or h2.
 *
 * wsi->buflist_comp stashes pre-transform input that was not yet compressed
 */

int
lws_http_compression_transform(struct lws *wsi, unsigned char *buf,
			       size_t len, enum lws_write_protocol *wp,
			       unsigned char **outbuf, size_t *olen_oused)
{
	size_t ilen_iused = len;
	int n, use = 0, wp1f = (*wp) & 0x1f;
	lws_comp_ctx_t *ctx = &wsi->http.comp_ctx;

	ctx->may_have_more = 0;

	if (!wsi->http.lcs ||
	    (wp1f != LWS_WRITE_HTTP && wp1f != LWS_WRITE_HTTP_FINAL)) {
		*outbuf = buf;
		*olen_oused = len;

		return 0;
	}

	if (wp1f == LWS_WRITE_HTTP_FINAL) {
		/*
		 * ...we may get a large buffer that represents the final input
		 * buffer, but it may form multiple frames after being
		 * tranformed by compression; only the last of those is actually
		 * the final frame on the output stream.
		 *
		 * Note that we have received the FINAL input, and downgrade it
		 * to a non-final for now.
		 */
		ctx->final_on_input_side = 1;
		*wp = LWS_WRITE_HTTP | ((*wp) & ~0x1f);
	}

	if (ctx->buflist_comp || ctx->may_have_more) {
		/*
		 * we can't send this new stuff when we have old stuff
		 * buffered and not compressed yet.  Add it to the tail
		 * and switch to trying to process the head.
		 */
		if (buf && len) {
			lws_buflist_append_segment(
				&ctx->buflist_comp, buf, len);
			lwsl_debug("%s: %p: adding %d to comp buflist\n",
				   __func__,wsi, (int)len);
		}

		len = lws_buflist_next_segment_len(&ctx->buflist_comp, &buf);
		ilen_iused = len;
		use = 1;
		lwsl_debug("%s: %p: trying comp buflist %d\n", __func__, wsi,
			   (int)len);
	}

	if (!buf && ilen_iused)
		return 0;

	lwsl_debug("%s: %p: pre-process: ilen_iused %d, olen_oused %d\n",
		   __func__, wsi, (int)ilen_iused, (int)*olen_oused);

	n = wsi->http.lcs->process(ctx, buf, &ilen_iused, *outbuf, olen_oused);

	if (n && n != 1) {
		lwsl_err("%s: problem with compression\n", __func__);

		return -1;
	}

	if (!ctx->may_have_more && ctx->final_on_input_side)
		*wp = LWS_WRITE_HTTP_FINAL | ((*wp) & ~0x1f);

	lwsl_debug("%s: %p: more %d, ilen_iused %d\n", __func__, wsi,
		   ctx->may_have_more, (int)ilen_iused);

	if (use && ilen_iused) {
		/*
		 * we were flushing stuff from the buflist head... account for
		 * however much actually got processed by the compression
		 * transform
		 */
		lws_buflist_use_segment(&ctx->buflist_comp, ilen_iused);
		lwsl_debug("%s: %p: marking %d of comp buflist as used "
			   "(ctx->buflist_comp %p)\n", __func__, wsi,
			   (int)len, ctx->buflist_comp);
	}

	if (!use && ilen_iused != len) {
		 /*
		  * ...we were sending stuff from the caller directly and not
		  * all of it got processed... stash on the buflist tail
		  */
		lws_buflist_append_segment(&ctx->buflist_comp,
					   buf + ilen_iused, len - ilen_iused);

		lwsl_debug("%s: buffering %d unused comp input\n", __func__,
			   (int)(len - ilen_iused));
	}
	if (ctx->buflist_comp || ctx->may_have_more)
		lws_callback_on_writable(wsi);

	return 0;
}
