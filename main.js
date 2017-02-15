"use strict";

process.env['NODE_TLS_REJECT_UNAUTHORIZED'] = '0'

var http_call = require("./http_call");

var url = "http://www.interpark.com/malls/index.html";
url = "http://ticket.interpark.com";	
//url = 'http://book.interpark.com/bookPark/html/book.html'

var start = Date.now();
http_call(url, (urls) => {
	var end = Date.now();

	urls.forEach((url) => {
		console.log(url);
	});

	console.log('urls:', urls.length, ', result :', (end - start) / 1000, 'sec');
});



