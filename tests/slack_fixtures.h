// Synthetic provider data shaped by lib/slack-inbox.js, lib/slack-routes.js and the catalog/docs
// inspected at nadinyamaui/briareus 9aabe649bb1835dd4ea50ebf3c9b3a997bde0e89 (PR #121, still open).
// These are contract fixtures, not evidence of live Slack/native QA or a final merged contract.
#ifndef BRIAREUS_SLACK_FIXTURES_H
#define BRIAREUS_SLACK_FIXTURES_H
#define SLACK_CONTRACT_SHA "9aabe649bb1835dd4ea50ebf3c9b3a997bde0e89"
#define SLACK_WORKSPACES "{\"workspaces\":[{\"id\":1727000000002,\"label\":\"Business\",\"teamName\":\"Example\",\"userId\":\"U1\",\"hasToken\":true,\"projects\":[]}]}"
#define SLACK_CONVERSATIONS "{\"conversations\":[{\"id\":\"C1\",\"name\":\"general\"},{\"id\":\"G1\",\"name\":\"private\",\"is_private\":true},{\"id\":\"D1\",\"user\":\"U1\",\"is_im\":true},{\"id\":\"G2\",\"name\":\"mpdm-team\",\"is_mpim\":true}],\"nextCursor\":\"\"}"
#define SLACK_PEOPLE "{\"people\":[{\"id\":\"U1\",\"name\":\"ana\",\"real_name\":\"Ana\",\"profile\":{\"display_name\":\"Ana Ops\"}}],\"nextCursor\":\"people-next\"}"
#define SLACK_EMPTY_PAGE "{\"messages\":[],\"nextCursor\":\"next\",\"hasMore\":true}"
#define SLACK_HISTORY "{\"messages\":[{\"ts\":\"1712345678.000002\",\"user\":\"U1\",\"text\":\"new\"},{\"ts\":\"1712345678.000001\",\"user\":\"U1\",\"text\":\"parent\",\"reply_count\":1}],\"nextCursor\":\"\",\"hasMore\":true}"
#define SLACK_THREAD "{\"messages\":[{\"ts\":\"1712345678.000001\",\"text\":\"parent\"},{\"ts\":\"1712345679.000001\",\"thread_ts\":\"1712345678.000001\",\"text\":\"reply\"}],\"nextCursor\":\"\",\"hasMore\":false}"
#define SLACK_RECEIPT "{\"channel\":\"C1\",\"ts\":\"1712345680.000001\",\"message\":{\"ts\":\"1712345680.000001\",\"user\":\"U1\",\"text\":\"human reply\"}}"
#define SLACK_CHANGED_RECEIPT "{\"channel\":\"C1\",\"ts\":\"1712345680.000001\",\"workspaceChanged\":true}"
#endif
