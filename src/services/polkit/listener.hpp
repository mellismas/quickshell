#pragma once

#include <qstring.h>

#define POLKIT_AGENT_I_KNOW_API_IS_SUBJECT_TO_CHANGE
// This causes a problem with variables of the name.
#undef signals

#include <glib-object.h>
#include <polkitagent/polkitagent.h>

#define signals Q_SIGNALS

#include "gobjectref.hpp"

namespace qs::service::polkit {
class ListenerCb;

//! A handler connected to a GCancellable. Holds a reference on the
//! cancellable and disconnects the handler when destroyed.
class CancelConnection {
public:
	CancelConnection() = default;
	~CancelConnection();
	Q_DISABLE_COPY_MOVE(CancelConnection);

	//! Connects `callback` to `cancellable`. Returns false if the cancellable
	//! was already cancelled, in which case the callback has already run.
	bool connect(GCancellable* cancellable, GCallback callback, gpointer data);

private:
	GObjectRef<GCancellable> cancellable;
	gulong handlerId = 0;
};

//! All state that comes in from PolKit about an authentication request.
struct AuthRequest {
	//! The action ID that this session is for.
	QString actionId;
	//! Message to present to the user.
	QString message;
	//! Icon name according to the FreeDesktop specification. May be empty.
	QString iconName;
	// Details intentionally omitted because nothing seems to use them.
	QString cookie;
	//! List of users/groups that can be used for authentication.
	std::vector<GObjectRef<PolkitIdentity>> identities;

	//! Implementation detail to mark authentication done.
	GObjectRef<GTask> task;
	//! Implementation detail for requests cancelled by agent. The cancellable
	//! is polkit's, so a handler left connected after the request is deleted
	//! would run on freed memory.
	CancelConnection cancelConnection;
	//! Callbacks for the listener
	ListenerCb* cb;

	void complete();
	void cancel(const QString& reason);
};

//! Callback interface for PolkitAgent listener events.
class ListenerCb {
public:
	ListenerCb() = default;
	virtual ~ListenerCb() = default;
	Q_DISABLE_COPY_MOVE(ListenerCb);

	//! Called when the agent registration is complete.
	virtual void registerComplete(bool success) = 0;
	//! Called when an authentication request is initiated by PolKit.
	virtual void initiateAuthentication(AuthRequest* request) = 0;
	//! Called when an authentication request is cancelled by PolKit before completion.
	virtual void cancelAuthentication(AuthRequest* request) = 0;
};
} // namespace qs::service::polkit

G_BEGIN_DECLS

// This is GObject code. By using their naming conventions, we clearly mark it
// as such for the rest of the project.
// NOLINTBEGIN(readability-identifier-naming)

#define QS_TYPE_POLKIT_AGENT (qs_polkit_agent_get_type())
G_DECLARE_FINAL_TYPE(QsPolkitAgent, qs_polkit_agent, QS, POLKIT_AGENT, PolkitAgentListener)

QsPolkitAgent* qs_polkit_agent_new(qs::service::polkit::ListenerCb* cb);
//! Forget the callback object. The listener can outlive it (a registration
//! in flight holds a reference to the listener, and polkit may still call
//! it); every call into the callback after this is skipped.
void qs_polkit_agent_detach(QsPolkitAgent* agent);
void qs_polkit_agent_register(QsPolkitAgent* agent, const char* path);
void qs_polkit_agent_unregister(QsPolkitAgent* agent);

// NOLINTEND(readability-identifier-naming)

G_END_DECLS
