
#pragma once

#include <iostream>
#include <istream>
#include <ostream>
#include <string>
#include <future>
#include <chrono>
#include <unordered_map>
#include <regex>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/bind.hpp>
#include <boost/regex.hpp>
#include <boost/algorithm/string.hpp>

namespace GLASS {

	using boost::asio::ip::tcp;
	using ms = std::chrono::milliseconds;
	using header_t = std::unordered_map < std::string, std::string > ;

	enum { HTTP_ERR = -1, HTTP_TIMEOUT = 0, HTTP_200 = 200 };


	class http_service
	{
	public:
		http_service() : work_(io_service_) 
		{
			std::thread tr([this]{
				io_service_.run();
			});
			tr.detach();
		}

		boost::asio::io_service& get_service()
		{
			return io_service_;
		}

	private:
		http_service(const http_service&);
		http_service& operator=(const http_service&);

		boost::asio::io_service io_service_;
		boost::asio::io_service::work work_;
	};

	class http_parser
	{
	private:
		http_parser() {}
		http_parser(const http_parser &p);

	public:

#if _MSC_VER >= 1800  // MSVC++ 12.0 _MSC_VER == 1800 (Visual Studio 2013)
		http_parser(http_parser &&p) : headers_(std::move(p.headers_)), body_(std::move(p.body_))
		{
		}
#else
		http_parser(http_parser &&p) : headers_(std::move(p.headers_)) 
		{
			body_ << p.body_.rdbuf();
		}
#endif
		header_t& header()
		{
			return headers_;
		}

		std::stringstream& body()
		{
			return body_;
		}

		static http_parser parse(std::stringstream &ss)
		{
			using namespace boost::algorithm;
			using std::string;

			http_parser this_;
			ss.seekg(0);

			std::vector<string> vs;
			string s;
			while (getline(ss, s) && s != "") {
				string::size_type pos = s.find(':');
				if (pos != string::npos) {
					string k = s.substr(0, pos);
					string v = s.substr(pos + 1);

					this_.headers_[std::move(k)] = trim_copy(v);
				}
			}

			while (getline(ss, s)) {
				this_.body_ << s << std::endl;
			}

			return std::move(this_);
		}

	private:
		header_t headers_;
		std::stringstream body_;
	};

	class http_client
	{
	public:
		http_client(http_service &svc)
			: service_(svc), resolver_(svc.get_service()), socket_(svc.get_service()),
			ctx_(boost::asio::ssl::context::sslv23), sslsocket_(socket_, ctx_)
		{
			//response_.prepare(81920);
		}

		~http_client()
		{
			close();
		}

		bool open(const std::string& url, const std::string& proxy = "", int timeout_ms = 30000)
		{
			bool r = false;
			timeout_ms_ = timeout_ms;
			resp_stream_.str("");
			headers_.clear();

			try {
				bool xssl = false;
				proxy_ = !proxy.empty();

				if (proxy_) {
					if (!urlparser(proxy)) {
						throw std::logic_error("error : invalid proxy url");
					}

					xhost_ = host_;
					xport_ = port_;
					xssl = ssl_;

					if (!urlparser(url)) {
						throw std::logic_error("error : invalid url");
					}

					ssl_ = xssl;
				}
				else {
					if (!urlparser(url)) {
						throw std::logic_error("error : invalid url");
					}

					xhost_ = host_;
					xport_ = port_;
				}


				if (ssl_) {
					ctx_.set_default_verify_paths();

					sslsocket_.set_verify_mode(boost::asio::ssl::verify_peer);
					sslsocket_.set_verify_callback([this](bool preverified, boost::asio::ssl::verify_context& ctx) -> bool
					{
						char subject_name[256];
						X509* cert = X509_STORE_CTX_get_current_cert(ctx.native_handle());
						X509_NAME_oneline(X509_get_subject_name(cert), subject_name, 256);
#ifdef BOOST_ASIO_ENABLE_HANDLER_TRACKING 
						std::cout << "Verifying: " << subject_name << "\n";
#endif
						return true || preverified;
					});
				}

				tcp::resolver::query query(xhost_, xport_);
				auto endpoint_iter = resolver_.resolve(query);

				boost::system::error_code err;
				boost::asio::connect(sslsocket_.lowest_layer(), endpoint_iter, err);
				
				if (err) {
					r = false;
					throw std::logic_error("connect error");
				}

				if (ssl_) {
					boost::system::error_code err;
					sslsocket_.handshake(boost::asio::ssl::stream_base::client, err);
				}

				r = true;
			}
			catch (...) {}
			return r;
		}

		void close()
		{
			if (is_open()) {
				sslsocket_.lowest_layer().close();
			}
		}

		bool is_open()
		{
			return sslsocket_.lowest_layer().is_open();
		}

		std::stringstream& response()
		{
			return resp_stream_;
		}

		void add_header(const std::string& k, const std::string& v)
		{
			headers_[k] = v;
		}

		void add_path(const std::string& path)
		{
			path_ += path;
		}

		std::future<int> head(const std::string &body = "")
		{
			return send("HEAD", body);
		}

		std::future<int> get(const std::string &body = "")
		{
			return send("GET", body);
		}

		std::future<int> post(const std::string &body = "")
		{
			return send("POST", body);
		}

		std::future<int> send(const std::string &method, const std::string &body = "")
		{
			method_ = method;
			data_ = body;

			async_write();

			timer_.reset(new boost::asio::deadline_timer(service_.get_service(), boost::posix_time::milliseconds(timeout_ms_)));
			timer_->async_wait([this](const boost::system::error_code err){
				if (!err) ret(HTTP_TIMEOUT);
			});

			return promise_->get_future();
		}


	private:

		void ret(int code)
		{
			if (timer_) {
				timer_->cancel();
				socket_.cancel();

				timer_.reset();
				socket_.close();

				promise_->set_value(code);
			}
		}

		bool urlparser(const std::string &url)
		{
			bool ret = false;

			boost::smatch m;
			boost::regex r(R"((https?):\/\/([^\/:]*):?([^\/]*)(\/?.*))");

			ret = boost::regex_search(url, m, r);

			if (ret) {
				protocol_ = boost::algorithm::to_lower_copy(m[1].str());
				host_ = m[2].str();
				port_ = m[3].str();

				ssl_ = (protocol_ == "https") ? true : false;

				if (port_.empty()) {
					port_ = (!ssl_) ? "80" : "443";
				}

				path_ = m[4].str();
				if (path_.empty()) path_ = "/";
			}

			return ret;
		}

		void async_write()
		{
			build_reqeust();
			promise_.reset(new std::promise<int>);;

			auto async_write_handler = [this](const boost::system::error_code &err, size_t len)
			{
				if (!err) {
					async_read_header();
				}
				else {
					close();
					ret(HTTP_ERR);
				}
			};

			if (ssl_) {
				boost::asio::async_write(sslsocket_, request_, async_write_handler);
			}
			else {
				boost::asio::async_write(socket_, request_, async_write_handler);
			}

		}

		void async_read_header()
		{
			using namespace boost::asio;

			auto async_read_until_handler = [this](const boost::system::error_code &err, std::size_t len)
			{
				// close 
				if (err) {
					close();
					ret(HTTP_ERR);
				}
				else {
					resp_stream_.str("");
					resp_stream_ << boost::algorithm::replace_all_copy(std::string(buffer_cast<const char*>(response_.data()), len), "\r\n", "\n");

					bool chunked = false;
					// header invalid check, get content-length 
					int content_length = parse_header(chunked);

					if (chunked) {
						content_length = parse_contents(chunked);
					}

					if (content_length > 0) {
						if (content_length > (int)response_.size()) {
							async_read_content(content_length - response_.size(), chunked);
						}
						else {
							parse_contents(chunked);
							ret(std::stoi(ret_code_));
						}
					}
					else {
						ret(std::stoi(ret_code_));
					}
				}
			};

			// buuffer initialize 
			response_.consume(response_.size());

			if (ssl_) {
				boost::asio::async_read_until(sslsocket_, response_, "\r\n\r\n", async_read_until_handler);
			}
			else {
				boost::asio::async_read_until(socket_, response_, "\r\n\r\n", async_read_until_handler);
			}

		}


		int parse_header(bool &chunked)
		{
			using std::string;
			using namespace boost::algorithm;
			int content_length = 0;

			try {
				std::istream response_stream(&response_);

				std::string header;
				getline(response_stream, header);

				std::vector<std::string> vs;
				split(vs, header, boost::is_any_of(" "), token_compress_on);

				if (vs.size() < 3) {
					std::ostringstream oss;
					oss << "Invalid http header 1 line, vs.size() :  " << vs.size() << std::endl;
					throw std::logic_error(oss.str());
				}

				string http_version = vs[0];
				ret_code_ = vs[1];
				//string status_msg = vs[2];

				if (!response_stream || http_version.substr(0, 5) != "HTTP/")
				{
					throw std::logic_error("Invalid http header version");
				}

				while (getline(response_stream, header) && header != "\r") {
					string::size_type pos = header.find("Content-Length");
					if (pos != string::npos) {
						pos = header.find(":");
						content_length = std::stoi(header.substr(pos + 1));
					}

					// Transfer-Encoding : chunked
					pos = header.find("Transfer-Encoding");
					if (pos != string::npos) {
						pos = header.find(":");

						if (string::npos != header.substr(pos + 1).find("chunked")) {
							chunked = true;
						}
					}
				}

				if (method_ == "HEAD") content_length = 0;

			}
			catch (std::exception &) {
				content_length = -1;
				//cout << e.what() << endl;
			}

			return content_length;
		}


		void async_read_content(size_t left, bool chunked = false)
		{
			auto async_read_handler = [this, left, chunked](const boost::system::error_code &err, std::size_t len)
			{
				using namespace boost::asio;

				if (!err) {
					if (chunked) {
						if (left > len) {
							resp_stream_ << std::string(buffer_cast<const char*>(response_.data()), response_.size());
							response_.consume(response_.size());

							async_read_content(left - len, chunked);
						}
						else {
							resp_stream_ << std::string(buffer_cast<const char*>(response_.data()), left);
							response_.consume(left);

							int content_length = parse_contents(chunked);

							if (content_length > 0) {
								async_read_content(content_length, chunked);
							}
							else {
								ret(std::stoi(ret_code_));
							}
						}
					}
					else {
						if (left > len) {
							resp_stream_ << std::string(buffer_cast<const char*>(response_.data()), response_.size());
							response_.consume(response_.size());

							async_read_content(left - len);
						}
						else {
							resp_stream_ << std::string(buffer_cast<const char*>(response_.data()), response_.size());
							response_.consume(response_.size());

							parse_contents();
							ret(std::stoi(ret_code_));
						}
					}
				}
				else if (err) {	// err
					if (err != boost::asio::error::eof) {
						close();
					}
					ret(HTTP_ERR);
				}
			};

			if (ssl_) {
				boost::asio::async_read(sslsocket_, response_, boost::asio::transfer_at_least(1), async_read_handler);
			}
			else {
				boost::asio::async_read(socket_, response_, boost::asio::transfer_at_least(1), async_read_handler);
			}
		}


		int parse_contents(bool chunked = false)
		{
			using std::string;
			using namespace boost::asio;
			int content_length = 0;

			try {
				if (chunked) {
					while (true) {
						if (response_.size() == 0) break;

						string s;
						std::istream response_stream(&response_);
						getline(response_stream, s);

						if (s == "\r") {
							getline(response_stream, s);
						}

						content_length = stoi(s, nullptr, 16);

						if (content_length == 0) {
							resp_stream_ << std::string(buffer_cast<const char*>(response_.data()), response_.size());
							response_.consume(response_.size());

							break;
						}
					
						if (content_length <= (int)response_.size()) {
							resp_stream_ << std::string(buffer_cast<const char*>(response_.data()), content_length);
							response_.consume(content_length);
						}
						else {
							content_length -= (int)response_.size();

							resp_stream_ << std::string(buffer_cast<const char*>(response_.data()), response_.size());
							response_.consume(response_.size());
							break;
						}
					}
				}
				else {
					// Content-Length : done
					resp_stream_ << std::string(buffer_cast<const char*>(response_.data()), response_.size());
					response_.consume(response_.size());
				}
			}
			catch (std::exception &) {}

			return content_length;
		}

		void build_reqeust()
		{
			if (method_.empty()) {
				method_ = "GET";
			}

			size_t content_length = data_.size();
			this->request_.consume(this->request_.size());

			std::ostream oss(&request_);

			if (proxy_) {
				// absoluteURI
				oss << method_ << " " << protocol_ << "://" << host_ << ":" << port_ << path_ << " HTTP/1.1\r\n";
			}
			else {
				//abs_path
				oss << method_ << " " << path_ << " HTTP/1.1\r\n";
			}
			oss << "Host: " << host_ << ":" << port_ << "\r\n";

			if (content_length > 0) {
				oss << "Content-Length: " << content_length << "\r\n";
			}

			if (headers_.end() == headers_.find("Accept")) {
				oss << "Accept: */*\r\n";
			}

			for (auto h : headers_) {
				oss << h.first << ": " << h.second << "\r\n";
			}
			oss << "\r\n";

			if (content_length > 0) {
				oss << data_;
			}

		}

	private:
		http_service& service_;

		tcp::resolver resolver_;
		tcp::socket socket_;
		boost::asio::ssl::context ctx_;
		boost::asio::ssl::stream<tcp::socket&> sslsocket_;

		boost::asio::streambuf request_;
		boost::asio::streambuf response_;
		std::stringstream resp_stream_;

		std::string xhost_;
		std::string xport_;
		int timeout_ms_;

		std::shared_ptr<std::promise<int>> promise_;
		std::shared_ptr<boost::asio::deadline_timer> timer_;
		std::string ret_code_;

		bool proxy_;
		bool ssl_;
		std::string protocol_;
		std::string host_;
		std::string port_;
		std::string path_;
		std::string method_;
		std::string data_;
		header_t headers_;
	};

}