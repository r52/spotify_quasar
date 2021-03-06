#include "spotifyapi.h"

#include <extension_support.h>

#include <QDesktopServices>
#include <QtNetworkAuth>

const QUrl apiUrl("https://api.spotify.com/v1/me/player");

SpotifyAPI::SpotifyAPI(quasar_ext_handle handle, QString clientid, QString clientsecret) :
    m_handle(handle), m_clientid(clientid), m_clientsecret(clientsecret), m_authenticated(false), m_granting(false), m_expired(false)
{
    if (nullptr == m_handle)
    {
        throw std::invalid_argument("null extension handle");
    }

    char buf[512];

    if (quasar_get_storage_string(handle, "refreshtoken", buf, sizeof(buf)))
    {
        m_refreshtoken = buf;
    }

    m_manager = new QNetworkAccessManager(this);
    m_oauth2  = new QOAuth2AuthorizationCodeFlow(m_manager, this);

    auto replyHandler = new QOAuthHttpServerReplyHandler(1337, this);
    replyHandler->setCallbackPath("callback");
    m_oauth2->setReplyHandler(replyHandler);
    m_oauth2->setClientIdentifier(m_clientid);
    m_oauth2->setClientIdentifierSharedKey(m_clientsecret);
    m_oauth2->setAuthorizationUrl(QUrl("https://accounts.spotify.com/authorize"));
    m_oauth2->setAccessTokenUrl(QUrl("https://accounts.spotify.com/api/token"));
    m_oauth2->setScope("user-read-currently-playing user-read-playback-state user-modify-playback-state user-read-recently-played");
    m_oauth2->setContentType(QAbstractOAuth::ContentType::Json);

    if (!m_refreshtoken.isEmpty())
    {
        m_oauth2->setRefreshToken(m_refreshtoken);
    }

    connect(m_oauth2, &QOAuth2AuthorizationCodeFlow::statusChanged, [=](QAbstractOAuth::Status status) {
        if (status == QAbstractOAuth::Status::Granted)
        {
            qInfo() << "SpotifyAPI: Authenticated.";
            m_authenticated = true;
            m_granting      = false;
        }
    });

    connect(m_oauth2, &QOAuth2AuthorizationCodeFlow::expirationAtChanged, [=](const QDateTime& expiration) {
        m_expired = (QDateTime::currentDateTime() > expiration);
    });

    connect(m_oauth2, &QOAuth2AuthorizationCodeFlow::authorizeWithBrowser, &QDesktopServices::openUrl);

    connect(m_oauth2, &QOAuth2AuthorizationCodeFlow::refreshTokenChanged, [=](const QString& refreshToken) {
        m_refreshtoken = refreshToken;

        auto ba = m_refreshtoken.toUtf8();
        quasar_set_storage_string(m_handle, "refreshtoken", ba.data());
    });

    m_oauth2->setModifyParametersFunction([&](QAbstractOAuth::Stage stage, QVariantMap* parameters) {
        if (stage == QAbstractOAuth::Stage::RefreshingAccessToken)
        {
            parameters->insert("client_id", m_clientid);
            parameters->insert("client_secret", m_clientsecret);
        }
    });
}

void SpotifyAPI::grant()
{
    if (m_clientid.isEmpty())
    {
        qWarning() << "SpotifyAPI: Client ID not set for authentication.";
        return;
    }

    if (!m_refreshtoken.isEmpty() && !m_clientsecret.isEmpty())
    {
        // Refresh token instead of granting if already granted
        qInfo() << "SpotifyAPI: Refreshing authorization tokens.";
        m_oauth2->refreshAccessToken();

        QTimer timer;
        timer.setSingleShot(true);
        QEventLoop loop;
        connect(m_oauth2, &QOAuth2AuthorizationCodeFlow::statusChanged, &loop, &QEventLoop::quit);
        connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
        timer.start(1000);
        loop.exec();

        // If authenticated by refreshtoken
        if (timer.isActive() && m_authenticated && !m_expired)
            return;
    }

    // If a grant already initiated, don't perform another one for a while
    if (m_granting)
        return;

    m_granting = true;
    // 1 minute grant timeout
    QTimer::singleShot(60000, [=]() { m_granting = false; });

    qInfo() << "SpotifyAPI: Obtaining Authorization grant.";
    m_oauth2->grant();
}

void SpotifyAPI::setClientIds(QString clientid, QString clientsecret)
{
    if (m_clientid != clientid)
    {
        m_clientid = clientid;
        m_oauth2->setClientIdentifier(m_clientid);
    }

    if (m_clientsecret != clientsecret)
    {
        m_clientsecret = clientsecret;
        m_oauth2->setClientIdentifierSharedKey(m_clientsecret);
    }
}

bool SpotifyAPI::execute(SpotifyAPI::Command cmd, quasar_data_handle output, QString args)
{
    // check expiry
    auto curr = QDateTime::currentDateTime();
    if (curr > m_oauth2->expirationAt())
    {
        // renew if token expired
        m_expired = true;
        grant();
    }

    if (!m_authenticated || m_expired)
    {
        qWarning() << "SpotifyAPI: Unauthenticated or expired access token";
        return false;
    }

    auto& dt = m_queue[cmd];

    if (dt.data_ready)
    {
        // set data
        if (dt.data.isEmpty())
        {
            quasar_set_data_null(output);
        }
        else
        {
            quasar_set_data_json(output, dt.data.data());
        }

        for (auto i : dt.errs)
        {
            quasar_append_error(output, i.toUtf8().data());
        }

        // clear queue and flags
        dt.data.clear();
        dt.errs.clear();

        dt.data_ready = false;
        dt.processing = false;
        return true;
    }

    if (dt.processing)
    {
        // still processing
        return true;
    }

    // otherwise, set processing
    dt.processing = true;

    // Process command
    const auto cmdinfo = m_infomap[cmd];

    auto oargs = QJsonDocument::fromJson(args.toUtf8()).object();

    QUrlQuery query;

    // Validate args
    convertArgToQuery(oargs, query, "device_id");

    switch (cmd)
    {
        case VOLUME:
        {
            if (!checkArgsForKey(oargs, "volume_percent", cmdinfo.src, output))
                return false;

            convertArgToQuery(oargs, query, "volume_percent");
            break;
        }

        case RECENTLY_PLAYED:
        {
            convertArgToQuery(oargs, query, "limit");
            convertArgToQuery(oargs, query, "after");
            convertArgToQuery(oargs, query, "before");
            break;
        }

        case REPEAT:
        {
            if (!checkArgsForKey(oargs, "state", cmdinfo.src, output))
                return false;

            convertArgToQuery(oargs, query, "state");
            break;
        }

        case SEEK:
        {
            if (!checkArgsForKey(oargs, "position_ms", cmdinfo.src, output))
                return false;

            convertArgToQuery(oargs, query, "position_ms");
            break;
        }

        case SHUFFLE:
        {
            if (!checkArgsForKey(oargs, "state", cmdinfo.src, output))
                return false;

            convertArgToQuery(oargs, query, "state");
            break;
        }

        default:
            break;
    }

    // Process args into data if any
    auto parameters = oargs.toVariantMap();

    // Create query url
    QUrl cmdurl = apiUrl.url() + cmdinfo.api + "?" + query.toString(QUrl::FullyEncoded);

    switch (cmdinfo.ptcl)
    {
        case GET:
        {
            QNetworkReply* reply = m_oauth2->get(cmdurl, parameters);
            connect(reply, &QNetworkReply::finished, [=]() {
                reply->deleteLater();

                auto& dt = m_queue[cmd];

                dt.data_ready = true;
                dt.processing = false;

                if (reply->error() != QNetworkReply::NoError)
                {
                    qWarning() << "SpotifyAPI:" << reply->error() << reply->errorString();
                    dt.errs.append(reply->errorString());
                    quasar_signal_data_ready(m_handle, cmdinfo.src.toUtf8().data());
                    return;
                }

                auto code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

                if (code != 204)
                {
                    const auto json = reply->readAll();
                    dt.data         = json;
                }

                quasar_signal_data_ready(m_handle, cmdinfo.src.toUtf8().data());
            });

            return true;
        }

        case PUT:
        case POST:
        {
            QNetworkReply* reply = (cmdinfo.ptcl == PUT ? m_oauth2->put(cmdurl, parameters) : m_oauth2->post(cmdurl, parameters));
            connect(reply, &QNetworkReply::finished, [=]() {
                reply->deleteLater();

                auto& dt = m_queue[cmd];

                dt.data_ready = true;
                dt.processing = false;

                if (reply->error() != QNetworkReply::NoError)
                {
                    qWarning() << "SpotifyAPI:" << reply->error() << reply->errorString();
                    dt.errs.append(reply->errorString());
                    quasar_signal_data_ready(m_handle, cmdinfo.src.toUtf8().data());
                    return;
                }

                auto code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

                if (code != 204)
                {
                    dt.errs.append(QString::number(code));
                }

                quasar_signal_data_ready(m_handle, cmdinfo.src.toUtf8().data());
            });

            return true;
        }
    }

    return false;
}

bool SpotifyAPI::checkArgsForKey(const QJsonObject& args, const QString& key, const QString& cmd, quasar_data_handle output)
{
    if (!args.contains(key))
    {
        qWarning() << "SpotifyAPI: Argument '" << key << "' required for the '" << cmd << "' endpoint.";
        QString m{"Argument '" + key + "' required."};
        quasar_append_error(output, m.toUtf8().data());
        return false;
    }

    return true;
}

void SpotifyAPI::convertArgToQuery(QJsonObject& args, QUrlQuery& query, const QString& convert)
{
    if (args.contains(convert))
    {
        auto v = args.take(convert);
        query.addQueryItem(convert, v.toString());
    }
}
