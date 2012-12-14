#include <libmary/libmary.h>

#include <mycpp/mycpp.h>
#include <mycpp/cmdline.h>

#include <cstdlib>


using namespace M;

namespace {

class Options
{
public:
    bool help;

    bool got_server_addr;
    IpAddress server_addr;

    Uint32 num_clients;
    Uint32 report_interval;

    Options ()
	: help (false),

	  got_server_addr (false),

	  num_clients (1),
	  report_interval (0)
    {
    }
};

mt_const Options options;

void printUsage ()
{
    outs->print ("Usage: bmeter_clnt [options]\n"
		 "Options:\n"
		 "  -s --server-addr <address>    Server address (default: localhost:7777)\n"
		 "  -n --num-clients <number>     Number of clients to simulate (default: 1)\n"
		 "  -r --report-interval <number> Interval between video frame reports.\n"
		 "                                0 means no reports. (default: 0)\n"
		 "  -h --help                     Show help message.\n");
    outs->flush ();
}

class ClientList_name;

class Client : public Object,
	       public IntrusiveListElement<ClientList_name>
{
public:
    bool valid;

    mt_const Byte id_char;

    TcpConnection tcp_conn;

    mt_mutex (::mutex) PollGroup::PollableKey pollable_key;

    Client (Byte const id_char)
	: valid (true),
	  id_char (id_char),
	  tcp_conn (this)
    {
    }
};

typedef IntrusiveList<Client, ClientList_name> ClientList;

mt_mutex (mutex) ClientList client_list;

Mutex mutex;

ServerApp server_app (NULL);

mt_const Byte *dummy_buf = NULL;
Size const dummy_buf_len = 1 << 20; // 1 Mb

mt_mutex (mutex) void destroyClient (Client * const client)
{
    if (!client->valid)
	return;
    client->valid = false;

    server_app.getMainThreadContext()->getPollGroup()->removePollable (client->pollable_key);

    client_list.remove (client);
    client->unref ();
}

void processInput (void * const _client)
{
    Client * const client = static_cast <Client*> (_client);

    // Discarding all data
    for (;;) {
	// TODO splice() to /dev/null
	Size nread = 0;
	AsyncIoResult const res = client->tcp_conn.read (Memory (dummy_buf, dummy_buf_len), &nread);
	if (res == AsyncIoResult::Error) {
	    logE_ (_func, "read() failed: ", exc->toString());
	    goto _destroy_client;
	}

	if (res == AsyncIoResult::Eof ||
	    res == AsyncIoResult::Normal_Eof)
	{
	    logW_ (_func, "EOF");
	    goto _destroy_client;
	}

	if (res == AsyncIoResult::Again ||
	    res == AsyncIoResult::Normal_Again)
	{
	    break;
	}

	assert (res == AsyncIoResult::Normal);
    }

    return;

_destroy_client:
    mutex.lock ();
    destroyClient (client);
    mutex.unlock ();
}

void processError (Exception * const exc_,
		   void      * const _client)
{
    Client * const client = static_cast <Client*> (_client);

    if (exc_)
	logD_ (_func, "exc: ", exc_->toString());
    else
	logD_ (_func_);

    mutex.lock ();
    destroyClient (client);
    mutex.unlock ();
}

Connection::InputFrontend const conn_input_frontend = {
    processInput,
    processError
};

void connected (Exception * const exc_,
		void      * const _client)
{
    Client * const client = static_cast <Client*> (_client);

    if (exc_) {
	logE_ (_func, "exc: ", exc_->toString());

	mutex.lock ();
	destroyClient (client);
	mutex.unlock ();
    } else
	logD_ (_func_);
}

TcpConnection::Frontend const tcp_conn_frontend = {
    connected
};

Result runClient ()
{
    Result ret_res = Result::Success;

    dummy_buf = new Byte [dummy_buf_len];

    if (!options.got_server_addr) {
	if (!setIpAddress ("127.0.0.1:7777", &options.server_addr)) {
	    logE_ (_func, "setIpAddress() failed");
	    ret_res = Result::Failure;
	    goto _return;
	}
    }

    if (!server_app.init ()) {
	logE_ (_func, "server_app.init() failed: ", exc->toString());
	ret_res = Result::Failure;
	goto _return;
    }

    {
	Byte id_char = 'a';
	for (Uint32 i = 0; i < options.num_clients; ++i) {
	    logD_ (_func, "starting client #", i);

	   Ref<Client> const client = new Client (id_char);

	   client->tcp_conn.setInputFrontend (
		   Cb<Connection::InputFrontend> (&conn_input_frontend, client, client));
	   client->tcp_conn.setFrontend (
		   Cb<TcpConnection::Frontend> (&tcp_conn_frontend, client, client));

           // TODO Check return value.
	   client->tcp_conn.connect (options.server_addr);

	   mutex.lock ();
	   client->pollable_key = server_app.getMainThreadContext()->getPollGroup()->addPollable (
		   client->tcp_conn.getPollable(), NULL /* ret_reg */);
	   if (!client->pollable_key) {
	       mutex.unlock ();
	       logE_ (_func, "PollGroup::addPollable() failed: ", exc->toString());
	       continue;
	   }

	   client_list.append (client);
	   mutex.unlock ();
	   client->ref ();

	   if (id_char == 'z')
	       id_char = 'a';
	   else
	       ++id_char;
	}
    }

    logI_ (_func, "Starting...");
    if (!server_app.run ())
	logE_ (_func, "server_app.run() failed: ", exc->toString());
    logI_ (_func, "...Finished");

    {
	ClientList::iter iter (client_list);
	while (!client_list.iter_done (iter)) {
	    Client * const client = client_list.iter_next (iter);
	    destroyClient (client);
	}
    }

_return:
    delete[] dummy_buf;
    dummy_buf = NULL;

    return ret_res;
}

bool cmdline_help (char const * /* short_name */,
		   char const * /* long_naem */,
		   char const * /* value */,
		   void       * /* opt_data */,
		   void       * /* cb_data */)
{
    options.help = true;
    return true;
}

bool cmdline_server_addr (char const * /* short_name */,
			  char const * /* long_name */,
			  char const *value,
			  void       * /* opt_data */,
			  void       * /* cb_data */)
{
    if (!setIpAddress_default (ConstMemory (value, strlen (value)),
			       "127.0.0.1" /* default_host */,
			       7777 /* default_port */,
			       false /* allow_any_host */,
			       &options.server_addr))
    {
	errs->print ("Invalid value \"", value, "\" "
		     "for --server-addr (IP:PORT expected)");
	exit (EXIT_FAILURE);
    }
    options.got_server_addr = true;
    return true;
}

bool cmdline_num_clients (char const * /* short_name */,
			  char const * /* long_name */,
			  char const *value,
			  void       * /* opt_data */,
			  void       * /* cb_data */)
{
    if (!strToUint32_safe (value, &options.num_clients)) {
	errs->print ("Invalid value \"", value, "\" "
		     "for --num-clients (number expected): ", exc->toString());
	exit (EXIT_FAILURE);
    }
    return true;
}

bool cmdline_report_interval (char const * /* short_name */,
			      char const * /* long_name */,
			      char const *value,
			      void       * /* opt_data */,
			      void       * /* cb_data */)
{
    if (!strToUint32_safe (value, &options.report_interval)) {
	errs->print ("Invalid value \"", value, "\" "
		     "for --report-interval (number expected): ", exc->toString());
	exit (EXIT_FAILURE);
    }
    return true;
}

}

int main (int    argc,
	  char **argv)
{
    MyCpp::myCppInit ();
    libMaryInit ();

    {
	unsigned const num_opts = 4;
	MyCpp::CmdlineOption opts [num_opts];

	opts [0].short_name   = "h";
	opts [0].long_name    = "help";
	opts [0].opt_callback = cmdline_help;

	opts [1].short_name   = "s";
	opts [1].long_name    = "server-addr";
	opts [1].with_value   = true;
	opts [1].opt_callback = cmdline_server_addr;

	opts [2].short_name   = "n";
	opts [2].long_name    = "num-clients";
	opts [2].with_value   = true;
	opts [2].opt_callback = cmdline_num_clients;

	opts [3].short_name   = "r";
	opts [3].long_name    = "report-interval";
	opts [3].with_value   = true;
	opts [3].opt_callback = cmdline_report_interval;

	MyCpp::ArrayIterator<MyCpp::CmdlineOption> opts_iter (opts, num_opts);
	MyCpp::parseCmdline (&argc, &argv, opts_iter, NULL /* callback */, NULL /* callback_data */);
    }

    if (options.help) {
	printUsage ();
	return 0;
    }

    if (runClient ())
	return 0;

    return EXIT_FAILURE;
}

