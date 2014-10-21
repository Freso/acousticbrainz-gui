#include <QDebug>
#include <QDir>
#include <QUrl>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QNetworkProxy>
#include <QNetworkProxyFactory>
#include <QDesktopServices>
#include <QMutexLocker>
#include <QThreadPool>
#include "loadfilelisttask.h"
#include "analyzefiletask.h"
#include "updatelogfiletask.h"
#include "extractor.h"
#include "constants.h"
#include "utils.h"
#include "gzip.h"

#include <qjson/parser.h>

class NetworkProxyFactory : public QNetworkProxyFactory
{
public:
	NetworkProxyFactory() : m_httpProxy(QNetworkProxy::NoProxy)
	{
		char* httpProxyUrl = getenv("http_proxy");
		if (httpProxyUrl) {
			QUrl url = QUrl::fromEncoded(QByteArray(httpProxyUrl));
			if (url.isValid() && !url.host().isEmpty()) {
				m_httpProxy = QNetworkProxy(QNetworkProxy::HttpProxy, url.host(), url.port(80));
				if (!url.userName().isEmpty())  {
					m_httpProxy.setUser(url.userName());
					if (!url.password().isEmpty()) {
						m_httpProxy.setPassword(url.password());
					}
				}
			}
		}
	}

	QList<QNetworkProxy> queryProxy(const QNetworkProxyQuery& query)
	{
		QList<QNetworkProxy> proxies = QNetworkProxyFactory::systemProxyForQuery(query);
		QString protocol = query.protocolTag().toLower();
		if (m_httpProxy.type() != QNetworkProxy::NoProxy && protocol == QLatin1String("http")) {
			QMutableListIterator<QNetworkProxy> i(proxies);
			while (i.hasNext()) {
				QNetworkProxy& proxy = i.next();
				if (proxy.type() == QNetworkProxy::NoProxy) {
					i.remove();
				}
			}
			proxies.append(m_httpProxy);
			proxies.append(QNetworkProxy::NoProxy);
		}
		return proxies;
	}

private:
	QNetworkProxy m_httpProxy;
};

Extractor::Extractor(const QStringList &directories, QTemporaryFile *profile)
	: m_profile(profile), m_directories(directories), m_paused(false), m_cancelled(false),
	  m_finished(false), m_reply(0), m_activeFiles(0), m_extractedFiles(0), m_submittedFiles(0)
{
	m_networkAccessManager = new QNetworkAccessManager(this);
	m_networkAccessManager->setProxyFactory(new NetworkProxyFactory());
	connect(m_networkAccessManager, SIGNAL(finished(QNetworkReply *)), SLOT(onRequestFinished(QNetworkReply*)));
}

Extractor::~Extractor()
{
}

void Extractor::start()
{
	m_time.start();
	LoadFileListTask *task = new LoadFileListTask(m_directories);
	connect(task, SIGNAL(finished(const QStringList &)), SLOT(onFileListLoaded(const QStringList &)), Qt::QueuedConnection);
	connect(task, SIGNAL(currentPathChanged(const QString &)), SIGNAL(currentPathChanged(const QString &)), Qt::QueuedConnection);
	task->setAutoDelete(true);
	emit fileListLoadingStarted();
	QThreadPool::globalInstance()->start(task);
}

void Extractor::pause()
{
	m_paused = true;
}

void Extractor::resume()
{
	m_paused = false;
	while (!m_files.isEmpty() && m_activeFiles < MAX_ACTIVE_FILES) {
		extractNextFile();
	}
	//maybeSubmit();
}

void Extractor::cancel()
{
	m_cancelled = true;
	m_files.clear();
	m_submitQueue.clear();
	if (m_reply) {
		m_reply->abort();
	}
	for (int i = 0; i < m_activeProcesses.size(); i++) {
		qDebug() << "process " << i << " terminating";
		m_activeProcesses[i]->terminate();
	}
}

bool Extractor::isPaused()
{
	return m_paused;
}

bool Extractor::hasErrors()
{
	return m_numErrors > 0;
}

bool Extractor::isCancelled()
{
	return m_cancelled;
}

bool Extractor::isFinished()
{
	return m_finished;
}

bool Extractor::isRunning()
{
	return !isPaused() && !isCancelled() && !isFinished();
}

void Extractor::onFileListLoaded(const QStringList &files)
{
	m_files = files;
	if (m_files.isEmpty()) {
		m_finished = true;
		emit noFilesError();
		emit finished();
		return;
	}
	emit extractionStarted(files.size());
	while (!m_files.isEmpty() && m_activeFiles < MAX_ACTIVE_FILES) {
		extractNextFile();
	}
}

void Extractor::extractNextFile()
{
	if (m_files.isEmpty()) {
		return;
	}
	m_activeFiles++;
	QString path = m_files.takeFirst();
	emit currentPathChanged(path);
	AnalyzeFileTask *task = new AnalyzeFileTask(path, m_profile->fileName());
	// TODO: How to remove this from the list when done
	m_activeProcesses.append(task);
	connect(task, SIGNAL(finished(AnalyzeResult *)), SLOT(onFileAnalyzed(AnalyzeResult *)), Qt::QueuedConnection);
	task->doanalyze();
}

void Extractor::onFileAnalyzed(AnalyzeResult *result)
{
	qDebug() << result->exitCode;
	qDebug() << result->outputFileName;
	m_activeFiles--;
	emit progress(++m_extractedFiles);
	if (!result->error) {
		if (!isCancelled()) {
			m_submitQueue.append(result);
		}
		if (isRunning()) {
			maybeSubmit();
		}
	}
	else {
		qDebug() << "Error" << result->errorMessage << "while processing" << result->fileName;
		m_numErrors++;
	}
	if (isRunning()) {
		extractNextFile();
	}
	if (m_activeFiles == 0 && m_files.isEmpty()) {
		if (m_submitQueue.isEmpty()) {
			m_finished = true;
			emit finished();
			return;
		}
		//maybeSubmit(true);
	}
}

bool Extractor::maybeSubmit(bool force) {
	if (!m_submitQueue.empty()) {
		AnalyzeResult *result = m_submitQueue.takeFirst();
		QString filename = result->outputFileName;
		QFile thejson(filename);
		if (thejson.open(QIODevice::ReadOnly)) {
			QJson::Parser parser;
			bool ok;
			QByteArray jsonContents = thejson.readAll();
			QVariantMap result = parser.parse (jsonContents, &ok).toMap();
			if (!ok) {
				qFatal("An error occurred during parsing");
				exit (1);
			}
			QVariantMap md = result["metadata"].toMap();
			QVariantMap tags = md["tags"].toMap();
			QString uuid;
			foreach (QVariant plugin, tags["musicbrainz_trackid"].toList()) {
				  qDebug() << "\t-" << plugin.toString();
				  uuid = plugin.toString();
			}

			QString submit = QString("http://localhost:8989/%1/low-level").arg(uuid);
			qDebug() << "Submitting to " << submit;
			QNetworkRequest request = QNetworkRequest(QUrl(submit));
			//request.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
			//request.setRawHeader("User-Agent", userAgentString().toAscii());
			// TODO This will read a QIODevice - a file?
			m_reply = m_networkAccessManager->post(request, jsonContents);
			return true;
		}
	}
}

void Extractor::onRequestFinished(QNetworkReply *reply)
{
}

