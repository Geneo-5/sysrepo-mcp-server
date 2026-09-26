/******************************************************************************
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * This file is part of sysrepo-mcp.
 ******************************************************************************/

#define _GNU_SOURCE

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <elog/elog.h>
#include <sysrepo.h>

#include <sysrepo/mcp/config.h>
#include <sysrepo/mcp/libconfig.h>
#include <sysrepo/mcp/log.h>

struct mcp_file_logger {
	struct elog super;
	FILE *file;
	enum elog_severity severity;
};

static struct elog_multi logger;
static struct elog_stdio console_logger;
static struct elog_syslog syslog_logger;
static struct mcp_file_logger file_logger;
static struct elog *active_logger;
static int logger_initialized;
static int multi_initialized;
static int console_initialized;
static int syslog_initialized;
static int file_initialized;

static void
file_vlog(struct elog *base, enum elog_severity severity,
	  const char *format, va_list args)
{
	struct mcp_file_logger *file = (struct mcp_file_logger *)base;
	char line[ELOG_LINE_MAX];

	if (severity == ELOG_CURRENT_SEVERITY)
		severity = file->severity;
	if (severity > file->severity)
		return;
	if (vsnprintf(line, sizeof(line), format, args) < 0)
		return;
	flockfile(file->file);
	fprintf(file->file, "%s %s: %s\n", CONFIG_PACKAGE_NAME,
	        elog_get_severity_label(severity), line);
	fflush(file->file);
	funlockfile(file->file);
}

static void
file_close(struct elog *base)
{
	struct mcp_file_logger *file = (struct mcp_file_logger *)base;

	if (file->file) {
		fclose(file->file);
		file->file = NULL;
	}
}

static const struct elog_ops file_ops = {
	.vlog = file_vlog,
	.close = file_close,
};

static void
sysrepo_log_callback(sr_log_level_t level, const char *message)
{
	enum elog_severity severity;

	switch (level) {
	case SR_LL_ERR:
		severity = ELOG_ERR_SEVERITY;
		break;
	case SR_LL_WRN:
		severity = ELOG_WARNING_SEVERITY;
		break;
	case SR_LL_INF:
		severity = ELOG_INFO_SEVERITY;
		break;
	case SR_LL_VRB:
	case SR_LL_DBG:
		severity = ELOG_DEBUG_SEVERITY;
		break;
	case SR_LL_NONE:
	default:
		return;
	}
	mcp_log(severity, "sysrepo: %s", message);
}

void
mcp_log_close(void)
{
	if (!logger_initialized)
		return;
	sr_log_set_cb(NULL);
	if (multi_initialized)
		elog_fini_multi(&logger);
	if (file_initialized)
		elog_fini(&file_logger.super);
	if (console_initialized)
		elog_fini_stdio(&console_logger);
	if (syslog_initialized)
		elog_fini_syslog(&syslog_logger);
	active_logger = NULL;
	logger_initialized = 0;
	multi_initialized = 0;
	console_initialized = 0;
	syslog_initialized = 0;
	file_initialized = 0;
}

int
mcp_log_init(const struct mcp_config *config)
{
	struct elog_syslog_conf syslog_conf = {
		.super.severity = config->log_level,
		.format = ELOG_PID_FMT,
		.facility = LOG_DAEMON,
	};
	struct elog_stdio_conf console_conf = {
		.super.severity = config->log_level,
		.format = ELOG_PID_FMT | ELOG_SEVERITY_FMT,
	};
	int rc;
	int file_error = 0;

	mcp_log_close();
	elog_init_multi(&logger, NULL);
	multi_initialized = 1;
	active_logger = elog_base(&logger);
	logger_initialized = 1;
	elog_setup(CONFIG_PACKAGE_NAME, getpid());

	if (config->syslog_enabled) {
		elog_init_syslog(&syslog_logger, &syslog_conf);
		syslog_initialized = 1;
		rc = elog_register_multi_sublog(&logger,
		                                elog_base(&syslog_logger));
		if (rc)
			goto fail;
	}
	if (config->log_console) {
		elog_init_stdio(&console_logger, &console_conf);
		console_initialized = 1;
		rc = elog_register_multi_sublog(&logger,
		                                elog_base(&console_logger));
		if (rc)
			goto fail;
	}
	if (config->log_file[0]) {
		file_logger.file = fopen(config->log_file, "a");
		if (!file_logger.file)
			file_error = errno;
		else {
			file_logger.super.ops = &file_ops;
			file_logger.severity = config->log_level;
			file_initialized = 1;
			rc = elog_register_multi_sublog(&logger,
			                                &file_logger.super);
			if (rc)
				goto fail;
		}
	}
	if (!logger.nr) {
		elog_init_stdio(&console_logger, &console_conf);
		console_initialized = 1;
		active_logger = elog_base(&console_logger);
	}
	if (file_error)
		mcp_log_warn("cannot open log file %s: %s", config->log_file,
		             strerror(file_error));
	sr_log_set_cb(sysrepo_log_callback);
	return 0;

fail:
	mcp_log_close();
	return rc;
}

void
mcp_log(enum elog_severity severity, const char *format, ...)
{
	char message[ELOG_LINE_MAX];
	va_list args;

	if (!active_logger)
		return;
	va_start(args, format);
	if (vsnprintf(message, sizeof(message), format, args) >= 0)
		elog_log(active_logger, severity, "%s", message);
	va_end(args);
}
