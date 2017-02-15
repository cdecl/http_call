// HttpCall.cpp : 콘솔 응용 프로그램에 대한 진입점을 정의합니다.
//

#include "stdafx.h"
#include <iostream>
#include <string>
#include <sstream>
#include <regex>
#include <vector>
#include <chrono>
#include <mutex>
#include <thread>
#include <algorithm>
using namespace std;

#include "http_client.h"

using vec_string = vector<string>;
using vec_future = vector<future<void>>;

void ParseHtml(vec_string &vUrl, string sHtml, const string &tag, const string &attr)
{
	ostringstream oss;
	oss << "<" << tag << ".*" << attr << R""(=["']([^"']*)")""s;
	regex re(oss.str());
	smatch m;

	while (regex_search(sHtml, m, re)) {
		vUrl.push_back(m[1]);
		sHtml = m.suffix().str();
	}
}

int Run(const string& service_url)
{
	mutex mtx;

	GLASS::http_service io;
	GLASS::http_client c(io);

	regex re(R""((http[s]?)://([^/]*).*)""s);
	smatch m;

	if (!regex_search(service_url, m, re)) {
		cout << "Invalid URL Parameter" << endl;
		return 0;
	}

	string protocol = m[1];
	string host = m[2];

	cout << protocol << " : " << host << endl;

	c.open(service_url);
	c.get().get();

	string sHtml = c.response().str();

	vec_string vUrl;
	vec_future vFuture;
	ParseHtml(vUrl, sHtml, "link", "href");
	ParseHtml(vUrl, sHtml, "script", "src");
	ParseHtml(vUrl, sHtml, "img", "src");

	auto it = std::unique(begin(vUrl), end(vUrl));
	vUrl.erase(it, end(vUrl));

	for (auto url : vUrl) {
		regex re(R""(http[s]?.*)""s);
		smatch m;

		string sUrl = url;
		
		if (!regex_match(url, m, re)) {
			if (sUrl.substr(0, 2) == "//") {
				sUrl = protocol + ":" + sUrl;
			}
			else if (sUrl.substr(0, 1) == "/") {
				sUrl = protocol + "://" + host + sUrl;
			}
		}

		auto f = async([&io, &mtx](string& url) {
			GLASS::http_client c(io);
			bool isopen = c.open(url, "", 5000);

			if (isopen) {
				c.get().get();
				{
					lock_guard<mutex> lock(mtx);
					cout << isopen << " : " << url << endl;
				}
			}
		}, sUrl);

		vFuture.push_back(std::move(f));
	}

	for (auto &f : vFuture) {
		f.wait();
	}	

	return vFuture.size();
}

int main()
{
	try {

		auto start = std::chrono::system_clock::now();

		int sites = Run("http://ticket.interpark.com");
		//int sites = Run("http://www.interpark.com/malls/index.html");

		std::chrono::duration<double> sec = std::chrono::system_clock::now() - start;
		cout << "Sites: " << sites <<", Time : " << sec.count() << endl;

	}
	catch (...) {
		cout << "unknown error" << endl;
	}

	return 0;

}

