#include <drogon/drogon.h>
#include "RuntimeResponse.h"
#include "RuntimeRequest.h"
#include "RuntimeOutput.h"
#include "{entrypointFile}"
#include <vector>
#include <numeric>

using namespace std;
using namespace runtime;
using namespace drogon;

vector<string> split(const string &s, char delim) {
    vector<string> result;
    stringstream ss (s);
    string item;

    while (getline (ss, item, delim)) {
        result.push_back (item);
    }

    return result;
}

int main()
{
    app()
        .addListener("0.0.0.0", 3000)
        .registerHandlerViaRegex(
            "/.*",
            [](const HttpRequestPtr &req,
               function<void(const HttpResponsePtr &)> &&callback)
            {
                const std::shared_ptr<HttpResponse> res = HttpResponse::newHttpResponse();

                auto secret = req->getHeader("x-open-runtimes-secret");
                if (secret.empty() || secret != std::getenv("OPEN_RUNTIMES_SECRET"))
                {
                    res->setStatusCode(static_cast<HttpStatusCode>(500));
                    res->setBody("Unauthorized. Provide correct \"x-open-runtimes-secret\" header.");
                    callback(res);
                    return;
                }

                auto method = req->getMethodString();
                auto path = req->getPath();
                auto queryString = req->getQuery();

                RuntimeRequest contextRequest;
                contextRequest.method = method;
                contextRequest.queryString = queryString;
                contextRequest.bodyString = req->getBody();
                contextRequest.body = contextRequest.bodyString;
                contextRequest.path = path;

                auto scheme = req->getHeader("x-forwarded-proto");
                if (scheme.empty())
                {
                    scheme = "http";
                }

                auto defaultPort = "80";
                if(scheme == "https") {
                    defaultPort = "443";
                }

                auto host = req->getHeader("host");
                auto port = std::stoi(defaultPort);

                if(host.empty())
                {
                    host = "";
                }

                if (host.find(":") != std::string::npos) {
                    vector<string> pair = split(host, ':');

                    if(pair.size() >= 2) {
                        host = pair[0];
                        port = std::stoi(pair[1]);
                    } else {
                        host = host;
                        port = std::stoi(defaultPort);
                    }
                } else {
                }

                contextRequest.scheme = scheme;
                contextRequest.host = host;
                contextRequest.port = port;

                auto url = scheme + "://" + host;

                if(port != std::stoi(defaultPort)) {
                    url += ":" + std::to_string(port);
                }

                url += path;

                if(!queryString.empty()) {
                    url += "?" + queryString;
                }

                contextRequest.url = url;

                Json::Value query;
                vector<string> params;

                if(std::string::npos != queryString.find('&'))
                {
                    params = split(queryString, '&');
                } else
                {
                    params.push_back(queryString);
                }

                for(auto param : params)
                {
                    if(std::string::npos != param.find('='))
                    {
                        vector<string> pairs = split(param, '=');
                        if(pairs.size() <= 1) {
                            query[pairs[0]] = "";
                        } else {
                            auto key = pairs[0];
                            pairs.erase(pairs.begin());

                            auto value = std::accumulate(
                                std::next(pairs.begin()), 
                                pairs.end(), 
                                pairs[0], 
                                [](std::string a, std::string b) {
                                    return a + "=" + b;
                                }
                            );
                            
                            query[key] = value.c_str();
                        }
                    } else
                    {
                        if(!param.empty()) {
                            query[param] = "";
                        }
                    }
                }

                if(query.empty()) {
                    Json::Value root;
                    Json::Reader reader;
                    reader.parse("{}", root);
                    query = root;
                }

                contextRequest.query = query;

                Json::Value headers;
                for(const auto header : req->getHeaders())
                {
                    auto headerKey = header.first;
                    std::transform(headerKey.begin(), headerKey.end(), headerKey.begin(), [](unsigned char c){ return std::tolower(c); });

                    if (headerKey.rfind("x-open-runtimes-", 0) != 0)
                    {
                        headers[headerKey] = req->getHeader(header.first);
                    }
                }

                contextRequest.headers = headers;

                auto contentType = req->getHeader("content-type");
                if(contentType.empty())
                {
                    contentType = "text/plain";
                }

                if (contentType.find("application/json") != std::string::npos)
                {
                    Json::Value bodyRoot;   
                    Json::Reader reader;
                    reader.parse(contextRequest.bodyString.c_str(), bodyRoot); 
                    contextRequest.body = bodyRoot;
                }

                RuntimeResponse contextResponse;

                RuntimeContext context;
                context.req = contextRequest;
                context.res = contextResponse;

                std::stringstream outbuffer;
                std::stringstream errbuffer;
                std::streambuf *oldout = std::cout.rdbuf(outbuffer.rdbuf());
                std::streambuf *olderr = std::cerr.rdbuf(errbuffer.rdbuf());

                RuntimeOutput output;
                try {
                    output = Handler::main(context);
                } catch(const std::exception& e)
                {
                    // TODO: Send trace to context.error()
                    context.error(e.what());
                    output = contextResponse.send("", 500, {});
                }

                // Should never be null. If somehow is, uncomment:
                /*
                if(output == NULL) {
                    context.error("Return statement missing. return context.res.empty() if no response is expected.");
                    output = contextResponse.send("", 500, {});
                }
                */

                if(!outbuffer.str().empty() || !errbuffer.str().empty())
                {
                    context.log("Unsupported log noticed. Use context.log() or context.error() for logging.");
                }

                std::cout.rdbuf(oldout);
                std::cerr.rdbuf(olderr);

                for (auto key : output.headers.getMemberNames())
                {
                    auto headerKey = key;
                    std::transform(headerKey.begin(), headerKey.end(), headerKey.begin(), [](unsigned char c){ return std::tolower(c); });

                    if (headerKey.rfind("x-open-runtimes-", 0) != 0)
                    {
                        res->addHeader(headerKey, output.headers[key].asString());
                    }
                }

                CURL *curl = curl_easy_init();

                if(context.logs.size() > 0)
                {
                    auto logsString = std::accumulate(
                        std::next(context.logs.begin()), 
                        context.logs.end(), 
                        context.logs[0], 
                        [](std::string a, std::string b) {
                            return a + "\n" + b;
                        }
                    );
                    char *logsEncoded = curl_easy_escape(curl, logsString.c_str(), logsString.length());
                    res->addHeader("x-open-runtimes-logs", logsEncoded);
                } else {
                    res->addHeader("x-open-runtimes-logs", "");
                }

                if(context.errors.size() > 0)
                {
                    auto errorsString = std::accumulate(
                        std::next(context.errors.begin()), 
                        context.errors.end(), 
                        context.errors[0], 
                        [](std::string a, std::string b) {
                            return a + "\n" + b;
                        }
                    );
                    char *errorsEncoded = curl_easy_escape(curl, errorsString.c_str(), errorsString.length());
                    res->addHeader("x-open-runtimes-errors", errorsEncoded);
                } else {
                    res->addHeader("x-open-runtimes-errors", "");
                }

                res->setStatusCode(static_cast<HttpStatusCode>(output.statusCode));
                res->setBody(output.body);
                callback(res);
            },
            {Post, Get, Put, Patch, Delete, Options})
        .run();
    return 0;
}

