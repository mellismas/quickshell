#pragma once

#include <qobject.h>
#include <qtmetamacros.h>

class TestPolkitListener: public QObject {
	Q_OBJECT;

private slots:
	static void cancelAfterUnansweredDelete();
	static void cancelAfterCompletion();
	static void deleteFromCancelHandler();
	static void alreadyCancelled();
	static void requestAfterDetach();
};
