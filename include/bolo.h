/*
  Copyright 2016 James Hunt <james@jameshunt.us>

  This file is part of Bolo.

  Bolo is free software: you can redistribute it and/or modify it under the
  terms of the GNU General Public License as published by the Free Software
  Foundation, either version 3 of the License, or (at your option) any later
  version.

  Bolo is distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
  details.

  You should have received a copy of the GNU General Public License along
  with Bolo.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef BOLO_H
#define BOLO_H

#define BOLO_VERSION "0.2.17"

#define OK       0
#define WARNING  1
#define CRITICAL 2
#define UNKNOWN  3
#define PENDING  4

typedef struct {
	char *name;
	char *value;
	int   wildcard;
} qname_part_t;

typedef struct {
	int           size;
	int           wildcard;
	qname_part_t* parts;
} qname_t;

pdu_t *parse_state_pdu  (int argc, char **argv, const char *ts);
pdu_t *parse_counter_pdu(int argc, char **argv, const char *ts);
pdu_t *parse_sample_pdu (int argc, char **argv, const char *ts);
pdu_t *parse_rate_pdu   (int argc, char **argv, const char *ts);
pdu_t *parse_setkeys_pdu(int argc, char **argv);
pdu_t *parse_event_pdu  (int argc, char **argv, const char *ts);
pdu_t *stream_pdu(const char *line);

pdu_t *forget_pdu  (uint16_t payload, const char *regex, uint8_t ignore);
pdu_t *state_pdu   (const char *name, int status, const char *msg);
pdu_t *counter_pdu (const char *name, unsigned int value);
pdu_t *sample_pdu  (const char *name, int n, ...);
pdu_t *rate_pdu    (const char *name, unsigned long value);
pdu_t *setkeys_pdu (int n, ...);
pdu_t *event_pdu   (const char *name, const char *extra);

/* qualified names */
qname_t* qname_parse(const char *name);
char*    qname_string(qname_t *name);
int      qname_match(qname_t *a, qname_t *b);

#endif
