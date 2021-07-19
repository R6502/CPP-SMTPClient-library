#include "securesmtpclientbase.h"
#include "smtpclienterrors.h"
#include "socketerrors.h"
#include "sslerrors.h"
#include <openssl/err.h>

#ifdef _WIN32
	#include <WinSock2.h>
    #include <ws2tcpip.h>
    #include <BaseTsd.h>
    typedef SSIZE_T ssize_t;
    #include <windows.h>
    constexpr auto sleep = Sleep;
#else
    #include <fcntl.h>
    #include <netdb.h>
    #include <unistd.h>
    #include <netinet/in.h>
    #include <openssl/bio.h> /* BasicInput/Output streams */
#endif

using namespace std;
using namespace jed_utils;

SecureSMTPClientBase::SecureSMTPClientBase(const char *pServerName, unsigned int pPort)
    : SMTPClientBase(pServerName, pPort),
      mBIO(nullptr),
      mCTX(nullptr),
      mSSL(nullptr)
{
}

SecureSMTPClientBase::~SecureSMTPClientBase()
{    
    cleanup();
}

//Copy constructor
SecureSMTPClientBase::SecureSMTPClientBase(const SecureSMTPClientBase& other)
	: SMTPClientBase(other),
      mBIO(nullptr),
      mCTX(nullptr),
      mSSL(nullptr)
{    
}

//Assignment operator
SecureSMTPClientBase& SecureSMTPClientBase::operator=(const SecureSMTPClientBase& other)
{
	if (this != &other) {
        SMTPClientBase::operator=(other);
        mBIO = nullptr;
        mCTX = nullptr;
        mSSL = nullptr;
	}
	return *this;
}

//Move constructor
SecureSMTPClientBase::SecureSMTPClientBase(SecureSMTPClientBase&& other) noexcept
	: SMTPClientBase(move(other)),
      mBIO(other.mBIO),
      mCTX(other.mCTX),
      mSSL(other.mSSL)
{
	// Release the data pointer from the source object so that the destructor 
	// does not free the memory multiple times.
    other.mBIO = nullptr;
    other.mCTX = nullptr;
    other.mSSL = nullptr;
}

//Move assignement operator
SecureSMTPClientBase& SecureSMTPClientBase::operator=(SecureSMTPClientBase&& other) noexcept
{
	if (this != &other) {
        SMTPClientBase::operator=(move(other));
		// Copy the data pointer and its length from the source object.
        mBIO = other.mBIO;
        mCTX = other.mCTX;
        mSSL = other.mSSL;
		// Release the data pointer from the source object so that
		// the destructor does not free the memory multiple times.
        other.mBIO = nullptr;
        other.mCTX = nullptr;
        other.mSSL = nullptr;
	}
	return *this;
}

void SecureSMTPClientBase::cleanup()
{
    if (mCTX != nullptr) {
        SSL_CTX_free(mCTX);
    }
    mCTX = nullptr;
    if (mBIO != nullptr) {
        BIO_free_all(mBIO);
    }
    mBIO = nullptr;
    if (mSock != 0) {
        #ifdef _WIN32
            shutdown(mSock, SD_BOTH);
            closesocket(mSock);
        #else
            close(mSock);
        #endif 
    }
    mSock = 0;
    mSSL = nullptr;
    #ifdef _WIN32
        WSACleanup();
    #endif
}


void SecureSMTPClientBase::initializeSSLContext()
{
    SSL_library_init();

    OpenSSL_add_all_algorithms();
    SSL_load_error_strings();
    const SSL_METHOD *method = TLS_client_method();
    mCTX = SSL_CTX_new(method);

    if (mCTX == nullptr) {
        mLastSocketErrNo = ERR_get_error();
    }
}

int SecureSMTPClientBase::startTLSNegotiation()
{
    addCommunicationLogItem("<Start TLS negotiation>");    
    initializeSSLContext();
    if (mCTX == nullptr) {
        return SSL_CLIENT_STARTTLS_INITSSLCTX_ERROR;
    }

    mBIO = BIO_new_ssl_connect(mCTX);
    if (mBIO == nullptr) {
        return SSL_CLIENT_STARTTLS_BIONEWSSLCONNECT_ERROR;
    }

    /* Link bio channel, SSL session, and server endpoint */
    char name[1024];
    sprintf(name, "%s:%i", getServerName(), getServerPort());
    BIO_get_ssl(mBIO, &mSSL); /* session */
    SSL_set_fd(mSSL, mSock);
    SSL_set_mode(mSSL, SSL_MODE_AUTO_RETRY); /* robustness */
    BIO_set_conn_hostname(mBIO, name); /* prepare to connect */

    #ifdef _WIN32
        /* On Windows, we need to import all the ROOT certificates to
        the OpenSSL Store */
        PCCERT_CONTEXT pContext = nullptr;
        X509_STORE *store = SSL_CTX_get_cert_store(mCTX);
        HCERTSTORE hStore = CertOpenSystemStore(NULL, "ROOT");

        if (!hStore) {
            mLastSocketErrNo = GetLastError();
            return SSL_CLIENT_STARTTLS_WIN_CERTOPENSYSTEMSTORE_ERROR;
        }

        while (pContext = CertEnumCertificatesInStore(hStore, pContext))
        {
            X509 *x509 = nullptr;
            x509 = d2i_X509(nullptr, (const unsigned char **)&pContext->pbCertEncoded, pContext->cbCertEncoded);
            if (x509)
            {
                X509_STORE_add_cert(store, x509);
                X509_free(x509);
            }
        }
    #else
        if (SSL_CTX_set_default_verify_paths(mCTX) == 0) {
           return SSL_CLIENT_STARTTLS_CTX_SET_DEFAULT_VERIFY_PATHS_ERROR; 
        }
    #endif
    long verify_flag = SSL_get_verify_result(mSSL);
    if (verify_flag != X509_V_OK) {
        fprintf(stderr,
            "##### Certificate verification error (%i) but continuing...\n",
            (int)verify_flag);
    }

    /* Try to connect */
    if (BIO_do_connect(mBIO) <= 0) {
        cleanup();
        mLastSocketErrNo = ERR_get_error();
        return SSL_CLIENT_STARTTLS_BIO_CONNECT_ERROR;
    }

    /* Try to do the handshake */
    addCommunicationLogItem("<Negotiate a TLS session>", "c & s");    
    if (BIO_do_handshake(mBIO) <= 0) {
        cleanup();
        mLastSocketErrNo = ERR_get_error();
        return SSL_CLIENT_STARTTLS_BIO_HANDSHAKE_ERROR;
    }

    addCommunicationLogItem("<Check result of negotiation>", "c & s");    
    /* Step 1: Verify a server certificate was presented 
       during the negotiation */
    X509* cert = SSL_get_peer_certificate(mSSL);
    if(cert != nullptr) { 
        X509_free(cert); /* Free immediately */
    } 
    if(cert == nullptr) {
        cleanup();
        return SSL_CLIENT_STARTTLS_GET_CERTIFICATE_ERROR;
    }

    /* Step 2: verify the result of chain verification */
    /* Verification performed according to RFC 4158    */
    int res = SSL_get_verify_result(mSSL);
    if(!(X509_V_OK == res)) {
        cleanup();
        return SSL_CLIENT_STARTTLS_VERIFY_RESULT_ERROR;
    }

    addCommunicationLogItem("TLS session ready!");    

    #ifdef _WIN32
        CertFreeCertificateContext(pContext);
        CertCloseStore(hStore, 0);
    #endif
    return 0;
}

int SecureSMTPClientBase::getServerSecureIdentification()
{
    const int EHLO_SUCCESS_CODE = 250;
    addCommunicationLogItem("Contacting the server again but via the secure channel...");
    string ehlo { "ehlo localhost\r\n"s };
    addCommunicationLogItem(ehlo.c_str());
    int tls_command_return_code = sendCommandWithFeedback(ehlo.c_str(), 
                                    SSL_CLIENT_INITSECURECLIENT_ERROR, 
                                    SSL_CLIENT_INITSECURECLIENT_TIMEOUT);
    if (tls_command_return_code != EHLO_SUCCESS_CODE) {
        return tls_command_return_code;
    }
    //Inspect the returned values for authentication options
    delete mAuthOptions;
    mAuthOptions = SMTPClientBase::extractAuthenticationOptions(mLastServerResponse);
    return EHLO_SUCCESS_CODE;
}

int SecureSMTPClientBase::sendCommand(const char *pCommand, int pErrorCode)
{
    if (const int status = BIO_puts(mBIO, pCommand) < 0) {
        mLastSocketErrNo = ERR_get_error();
        cleanup();
        return pErrorCode;
    }
    return 0;
}

int SecureSMTPClientBase::sendCommandWithFeedback(const char *pCommand, int pErrorCode, int pTimeoutCode)
{
    unsigned int waitTime {0};
    int bytes_received {0};
    char outbuf[1024];

    if (const int status = BIO_puts(mBIO, pCommand) < 0) {
        mLastSocketErrNo = ERR_get_error();
        cleanup();
        return pErrorCode;
    }
    
    while ((bytes_received = BIO_read(mBIO, outbuf, 1024)) <= 0 && waitTime < mCommandTimeOut) {
        sleep(1);
        waitTime += 1;
    }
    if (waitTime < mCommandTimeOut) {
        outbuf[bytes_received-1] = '\0';
        setLastServerResponse(outbuf);
        addCommunicationLogItem(outbuf, "s");
        return extractReturnCode(outbuf);
    }
    
    cleanup();
    return pTimeoutCode;  
}