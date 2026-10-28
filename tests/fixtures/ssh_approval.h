#ifndef BRIAREUS_FIXTURE_SSH_APPROVAL_H
#define BRIAREUS_FIXTURE_SSH_APPROVAL_H
#define SSH_REQUEST "{\"id\":\"fake-request\",\"repo\":\"one/repo\",\"jobId\":\"fake-session\",\"sessionTitle\":\"Fixture session\",\"serverId\":7,\"serverLabel\":\"Fixture server\",\"host\":\"fixture.invalid\",\"port\":22,\"username\":\"operator\",\"command\":\"printf fixture\",\"status\":\"pending\",\"expiresAt\":4102444800000,\"timeoutSeconds\":60}"
#define SSH_REQUESTS "{\"requests\":[" SSH_REQUEST "]}"
#endif
