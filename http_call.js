"use strict";

var async = require("async");
var http = require("http");
var https = require("https");
var _ = require("underscore");

var http_call = (url, callback_main) => {
	const re = /(http[s]?):\/\/([^\/]*).*/g;

	if (!re.test(url)) {
		console.error("Invalid URL Parameter");
		return;
	}

	re.lastIndex = 0;
	var m = re.exec(url);
	var protocol = m[1];
	var host = m[2];

	console.log("protocol:", protocol, ', host:', host);

	var GetHttp = (protocol) => {
		var proto_o = { "http" : http, "https" : https }
		return proto_o[protocol];
	};

	GetHttp(protocol).get(url, (resp) => {
		var str = "";
		resp.on('data', (chunk) => { str += chunk; }); 
		resp.on('end', () => { 
			http_parse(str);
		});
	});

	var http_parse = (str) => {
		var urls = [];
		urls = urls.concat(extract_url(str, "link", "href"));
		urls = urls.concat(extract_url(str, "script", "src"));
		urls = urls.concat(extract_url(str, "img", "src"));
		urls = _.unique(urls)

		var async_call = (u, callback) => {
			//console.info(u);
			var call_url = u;

			var re = /^\/\/.*/g;	// "//abc"
			if (re.test(u)) {
				call_url = protocol + ":" + u;
			}

			re = /^\/[^\/].*/g;  // "/abc"
			if (re.test(u)) {
				call_url = protocol + "://" + host + u;
			}

			http_get(call_url, callback);
		};

		async.map(urls, async_call, (err, results) => {
			if (callback_main) callback_main(urls);
		});

	};

	var http_get = (url, callback) => {
		// console.log(url)
		const re = /^(http[s]?).*/g; 

		if (!re.test(url)) {
			callback(null, url);
			return;
		}

		re.lastIndex = 0;
		var protocol = re.exec(url)[1];

		GetHttp(protocol).get(url, function (resp) {
			resp.on('data', (chunk) => {});
			resp.on('end', () => {
				//console.log('end : ', '', ' : ', url);
				callback(null, url);
			});
		}).on('error', (e) => {
			console.error('error : ', e);
		});
	};

	var extract_url = (str, tag, attr) => {
		const re = new RegExp("<" + tag + ".*" + attr + "=[\"']([^\"']*)", "g")
		var m, urls = [];

		while ((m = re.exec(str)) != null) {
			urls.push(m[1]);
			re.lastIndex++;
		}

		return urls;
	};

};


module.exports = http_call;
