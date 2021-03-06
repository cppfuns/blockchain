// Copyright (c) 2009-2015 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_WALLET_CRYPTER_H
#define BITCOIN_WALLET_CRYPTER_H

#include "keystore.h"
#include "serialize.h"
#include "support/allocators/secure.h"

class uint256;

const unsigned int WALLET_CRYPTO_KEY_SIZE = 32; // 钱包加密密码大小
const unsigned int WALLET_CRYPTO_SALT_SIZE = 8; // 钱包加密盐值大小

/**
 * Private key encryption is done based on a CMasterKey,
 * which holds a salt and random encryption key.
 * 
 * CMasterKeys are encrypted using AES-256-CBC using a key
 * derived using derivation method nDerivationMethod
 * (0 == EVP_sha512()) and derivation iterations nDeriveIterations.
 * vchOtherDerivationParameters is provided for alternative algorithms
 * which may require more parameters (such as scrypt).
 * 
 * Wallet Private Keys are then encrypted using AES-256-CBC
 * with the double-sha256 of the public key as the IV, and the
 * master key's key as the encryption key (see keystore.[ch]).
 */

/** Master key for wallet encryption */
class CMasterKey // 钱包加密的主密钥
{
public:
    std::vector<unsigned char> vchCryptedKey; // 加密的密钥
    std::vector<unsigned char> vchSalt; // 盐值
    //! 0 = EVP_sha512()
    //! 1 = scrypt()
    unsigned int nDerivationMethod; // 派生方式
    unsigned int nDeriveIterations; // 派生迭代计数
    //! Use this for more parameters to key derivation,
    //! such as the various parameters to scrypt
    std::vector<unsigned char> vchOtherDerivationParameters;

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action, int nType, int nVersion) {
        READWRITE(vchCryptedKey);
        READWRITE(vchSalt);
        READWRITE(nDerivationMethod);
        READWRITE(nDeriveIterations);
        READWRITE(vchOtherDerivationParameters);
    }

    CMasterKey()
    {
        // 25000 rounds is just under 0.1 seconds on a 1.86 GHz Pentium M
        // ie slightly lower than the lowest hardware we need bother supporting
        nDeriveIterations = 25000;
        nDerivationMethod = 0;
        vchOtherDerivationParameters = std::vector<unsigned char>(0);
    }
};

typedef std::vector<unsigned char, secure_allocator<unsigned char> > CKeyingMaterial;

/** Encryption/decryption context with key information */
class CCrypter // 使用密钥信息加密/解密上下文
{
private:
    unsigned char chKey[WALLET_CRYPTO_KEY_SIZE]; // 对称密钥
    unsigned char chIV[WALLET_CRYPTO_KEY_SIZE]; // 初始化向量 iv
    bool fKeySet;

public:
    bool SetKeyFromPassphrase(const SecureString &strKeyData, const std::vector<unsigned char>& chSalt, const unsigned int nRounds, const unsigned int nDerivationMethod); // 使用 sha512 进行加密，获取密钥和初始化的 iv
    bool Encrypt(const CKeyingMaterial& vchPlaintext, std::vector<unsigned char> &vchCiphertext); // 加密
    bool Decrypt(const std::vector<unsigned char>& vchCiphertext, CKeyingMaterial& vchPlaintext);
    bool SetKey(const CKeyingMaterial& chNewKey, const std::vector<unsigned char>& chNewIV);

    void CleanKey()
    {
        memory_cleanse(chKey, sizeof(chKey));
        memory_cleanse(chIV, sizeof(chIV));
        fKeySet = false;
    }

    CCrypter()
    {
        fKeySet = false;

        // Try to keep the key data out of swap (and be a bit over-careful to keep the IV that we don't even use out of swap)
        // Note that this does nothing about suspend-to-disk (which will put all our key data on disk)
        // Note as well that at no point in this program is any attempt made to prevent stealing of keys by reading the memory of the running process.
        LockedPageManager::Instance().LockRange(&chKey[0], sizeof chKey);
        LockedPageManager::Instance().LockRange(&chIV[0], sizeof chIV);
    }

    ~CCrypter()
    {
        CleanKey();

        LockedPageManager::Instance().UnlockRange(&chKey[0], sizeof chKey);
        LockedPageManager::Instance().UnlockRange(&chIV[0], sizeof chIV);
    }
};

/** Keystore which keeps the private keys encrypted.
 * It derives from the basic key store, which is used if no encryption is active.
 */ // 用于存储加密私钥的密钥库。
class CCryptoKeyStore : public CBasicKeyStore
{
private:
    CryptedKeyMap mapCryptedKeys; // 密钥索引对应公钥私钥对映射列表

    CKeyingMaterial vMasterKey; // 主密钥

    //! if fUseCrypto is true, mapKeys must be empty
    //! if fUseCrypto is false, vMasterKey must be empty
    bool fUseCrypto; // 如果使用加密标志为 true，mapKeys 必须为空；如果为 false，vMasterKey 必须为空

    //! keeps track of whether Unlock has run a thorough check before
    bool fDecryptionThoroughlyChecked; // 跟踪 在解锁前是否运行过一个彻底的检查 的标志

protected:
    bool SetCrypted();

    //! will encrypt previously unencrypted keys
    bool EncryptKeys(CKeyingMaterial& vMasterKeyIn); // 加密之前未加密的密钥

    bool Unlock(const CKeyingMaterial& vMasterKeyIn);

public:
    CCryptoKeyStore() : fUseCrypto(false), fDecryptionThoroughlyChecked(false)
    {
    }

    bool IsCrypted() const // 返回当前钱包是否被用户加密的状态
    {
        return fUseCrypto;
    }

    bool IsLocked() const
    {
        if (!IsCrypted()) // fUseCrypto 为 false
            return false;
        bool result; // fUseCrypto 为 true
        {
            LOCK(cs_KeyStore);
            result = vMasterKey.empty();
        }
        return result;
    }

    bool Lock();

    virtual bool AddCryptedKey(const CPubKey &vchPubKey, const std::vector<unsigned char> &vchCryptedSecret);
    bool AddKeyPubKey(const CKey& key, const CPubKey &pubkey);
    bool HaveKey(const CKeyID &address) const // 检查 KeyID 对应的密钥是否存在
    {
        {
            LOCK(cs_KeyStore);
            if (!IsCrypted()) // 若当前钱未被用户加密
                return CBasicKeyStore::HaveKey(address); // 检查公钥 ID 对应的私钥是否存在
            return mapCryptedKeys.count(address) > 0; // 存在 1 > 0，返回 true，反之，返回 false
        }
        return false;
    }
    bool GetKey(const CKeyID &address, CKey& keyOut) const; // 通过密钥索引获取私钥
    bool GetPubKey(const CKeyID &address, CPubKey& vchPubKeyOut) const; // 通过密钥索引获取公钥
    void GetKeys(std::set<CKeyID> &setAddress) const // 获取密钥索引集合
    {
        if (!IsCrypted())
        {
            CBasicKeyStore::GetKeys(setAddress);
            return;
        }
        setAddress.clear();
        CryptedKeyMap::const_iterator mi = mapCryptedKeys.begin();
        while (mi != mapCryptedKeys.end())
        {
            setAddress.insert((*mi).first);
            mi++;
        }
    }

    /**
     * Wallet status (encrypted, locked) changed.
     * Note: Called without locks held.
     */ // 钱包状态（加密，锁定）改变。注：无锁时调用。
    boost::signals2::signal<void (CCryptoKeyStore* wallet)> NotifyStatusChanged;
};

#endif // BITCOIN_WALLET_CRYPTER_H
