/**
*   Copyright(C) 2011-2015 Intel Corporation All Rights Reserved.
*
*   The source code, information  and  material ("Material") contained herein is
*   owned  by Intel Corporation or its suppliers or licensors, and title to such
*   Material remains  with Intel Corporation  or its suppliers or licensors. The
*   Material  contains proprietary information  of  Intel or  its  suppliers and
*   licensors. The  Material is protected by worldwide copyright laws and treaty
*   provisions. No  part  of  the  Material  may  be  used,  copied, reproduced,
*   modified, published, uploaded, posted, transmitted, distributed or disclosed
*   in any way  without Intel's  prior  express written  permission. No  license
*   under  any patent, copyright  or  other intellectual property rights  in the
*   Material  is  granted  to  or  conferred  upon  you,  either  expressly,  by
*   implication, inducement,  estoppel or  otherwise.  Any  license  under  such
*   intellectual  property  rights must  be express  and  approved  by  Intel in
*   writing.
*
*   *Third Party trademarks are the property of their respective owners.
*
*   Unless otherwise  agreed  by Intel  in writing, you may not remove  or alter
*   this  notice or  any other notice embedded  in Materials by Intel or Intel's
*   suppliers or licensors in any way.
*/


#include "service_provider.h"

#include "sample_libcrypto.h"

#include "ecp.h"

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <time.h>
#include <string.h>
#ifdef _MSC_VER
#include "stdafx.h"
#endif
#include "ias_ra.h"

#ifndef SAFE_FREE
#define SAFE_FREE(ptr) {if (NULL != (ptr)) {free(ptr); (ptr) = NULL;}}
#endif

#define SAMPLE_RA_KEY_SMK 0

// This is the private EC key of SP, the corresponding public EC key is
// hard coded in isv_enclave. It is based on NIST P-256 curve.
static const sample_ec256_private_t g_sp_priv_key = {{
    0x39, 0x30, 0x00, 0x00, 0x7e, 0x16, 0x00, 0x00,
    0xdf, 0x27, 0x00, 0x00, 0x2c, 0x1c, 0x00, 0x00,
    0xf5, 0x16, 0x00, 0x00, 0x8a, 0x1c, 0x00, 0x00,
    0xfb, 0x62, 0x00, 0x00, 0x18, 0x4f, 0x00, 0x00
}};

// This is the public EC key of SP, this key is hard coded in isv_enclave.
// It is based on NIST P-256 curve. Not used in the SP code.
static const sample_ec_pub_t g_sp_pub_key = {{
    0x46, 0x09, 0x1f, 0x1e, 0x25, 0x00, 0x29, 0x59,
    0x70, 0xac, 0xce, 0xbb, 0xd7, 0xa6, 0x68, 0xc2,
    0x30, 0x42, 0x4a, 0xae, 0xaa, 0x59, 0x64, 0x3a,
    0x33, 0xba, 0x7f, 0xca, 0x89, 0x52, 0xf7, 0x64},
   {0x60, 0x28, 0xc3, 0xcf, 0x5a, 0x3b, 0x66, 0x4a,
    0x46, 0xd1, 0x63, 0x54, 0xa2, 0x9f, 0xd0, 0xad,
    0xa0, 0xa6, 0x08, 0x0b, 0xec, 0x85, 0x9d, 0x76,
    0xf1, 0x0b, 0x53, 0xa1, 0xeb, 0x11, 0xb3, 0x2d
}};

// This is a context data structure used on SP side
typedef struct _sp_db_item_t
{
    sample_ec_pub_t             g_a;
    sample_ec_pub_t             g_b;
    sample_ec_key_128bit_t      vk_key;// Shared secret key for the REPORT_DATA
    sample_ec_key_128bit_t      mk_key;// Shared secret key for generating MAC's
    sample_ec_key_128bit_t      sk_key;// Shared secret key for encryption
    sample_ec_key_128bit_t      smk_key;// Used only for SIGMA protocol
    sample_ec_priv_t            b;
    sample_ps_sec_prop_desc_t   ps_sec_prop;
}sp_db_item_t;
static sp_db_item_t g_sp_db;

static bool g_is_sp_registered = false;
static int g_sp_credentials = 0;
static int g_authentication_token = 0;

uint8_t g_secret[8] = {0,1,2,3,4,5,6,7};

sample_spid_t g_spid;

// Verify message 1 then generate and return message 2 to isv.
int sp_ra_proc_msg1_req(const sample_ra_msg1_t *p_msg1,
						uint32_t msg1_size,
						ra_samp_response_header_t **pp_msg2)
{
    int ret = 0;
    ra_samp_response_header_t* p_msg2_full = NULL;
    sample_ra_msg2_t *p_msg2 = NULL;
    sample_ecc_state_handle_t ecc_state = NULL;
    sample_status_t sample_ret = SAMPLE_SUCCESS;
    bool derive_ret = false;

    if(!p_msg1 ||
	   !pp_msg2 ||
	   (msg1_size != sizeof(sample_ra_msg1_t)))
	{
        return -1;
	}

    do
    {
        // Check to see if we have registered with the IAS yet?
        if(!g_is_sp_registered)
        {
            do
            {
                // @IAS_Q: What are the sp credentials?
                // @IAS_Q: What is in the authentication token
                // In the product, the SP will establish a mutually
                // authenticated SSL channel. The authentication token is
                // based on this channel.
                // @TODO: Convert this call to a 'network' send/receive
                // once the IAS server is a vaialable.
                ret = ias_enroll(g_sp_credentials, &g_spid,
                                 &g_authentication_token);
                if(0 != ret)
                {
                    ret = SP_IAS_FAILED;
                    break;
                }

                // IAS may support registering the Enclave Trust Policy.
                // Just leave a place holder here
                // @IAS_Q: What needs to be sent to the IAS with the policy
                // that identifies the SP?
                // ret = ias_register_enclave_policy(g_enclave_policy,
                // g_authentication_token);
                // if(0 != ret)
                // {
                //     break;
                // }

                g_is_sp_registered = true;
                break;
            } while(0);
        }

        // Get the sig_rl from IAS using GID.
        // GID is Base-16 encoded of EPID GID in little-endian format.
        // @IAS_Q: Does the SP need to supply any authentication info to the
        // IAS?  SPID?
        // In the product, the SP and IAS will use an established channel for
        // communication.
        uint8_t* sig_rl;
        uint32_t sig_rl_size = 0;

        // @TODO: Convert this call to a 'network' send/receive
        // once the IAS server is a vaialable.
        ret = ias_get_sigrl(p_msg1->gid, &sig_rl_size, &sig_rl);
        if(0 != ret)
        {
            fprintf(stderr, "\nError, ias_get_sigrl [%s].", __FUNCTION__);
			ret = SP_IAS_FAILED;
            break;
        }

        // Need to save the client's public ECCDH key to local storage
        if (memcpy_s(&g_sp_db.g_a, sizeof(g_sp_db.g_a), &p_msg1->g_a,
                     sizeof(p_msg1->g_a)))
        {
            fprintf(stderr, "\nError, cannot do memcpy in [%s].", __FUNCTION__);
            ret = SP_INTERNAL_ERROR;
            break;
        }

        // Generate the Service providers ECCDH key pair.
        sample_ret = sample_ecc256_open_context(&ecc_state);
        if(SAMPLE_SUCCESS != sample_ret)
        {
            fprintf(stderr, "\nError, cannot get ECC cotext in [%s].",
                             __FUNCTION__);
            ret = -1;
            break;
        }
        sample_ec256_public_t pub_key = {{0},{0}};
        sample_ec256_private_t priv_key = {{0}};
        sample_ret = sample_ecc256_create_key_pair(&priv_key, &pub_key,
                                                   ecc_state);
        if(SAMPLE_SUCCESS != sample_ret)
        {
            fprintf(stderr, "\nError, cannot generate key pair in [%s].",
                    __FUNCTION__);
            ret = SP_INTERNAL_ERROR;
            break;
        }

        // Need to save the SP ECCDH key pair to local storage.
        if(memcpy_s(&g_sp_db.b, sizeof(g_sp_db.b), &priv_key,sizeof(priv_key))
           || memcpy_s(&g_sp_db.g_b, sizeof(g_sp_db.g_b),
                       &pub_key,sizeof(pub_key)))
        {
            fprintf(stderr, "\nError, cannot do memcpy in [%s].", __FUNCTION__);
            ret = SP_INTERNAL_ERROR;
            break;
        }

        // Generate the client/SP shared secret
        sample_ec_dh_shared_t dh_key = {{0}};
        sample_ret = sample_ecc256_compute_shared_dhkey(&priv_key,
            (sample_ec256_public_t *)&p_msg1->g_a,
            (sample_ec256_dh_shared_t *)&dh_key,
            ecc_state);
        if(SAMPLE_SUCCESS != sample_ret)
        {
            fprintf(stderr, "\nError, compute share key fail in [%s].",
                    __FUNCTION__);
            ret = SP_INTERNAL_ERROR;
            break;
        }

        // smk is only needed for msg2 generation.
        derive_ret = derive_key(&dh_key, SAMPLE_RA_KEY_SMK,
                                (uint8_t*)&g_sp_db.smk_key);
        if(derive_ret != true)
        {
            fprintf(stderr, "\nError, derive key fail in [%s].", __FUNCTION__);
            ret = SP_INTERNAL_ERROR;
            break;
        }

		// The rest of the keys are the shared secrets for future communication.
        derive_ret = derive_key(&dh_key, SAMPLE_RA_KEY_MK,
                                (uint8_t*)&g_sp_db.mk_key);
        if(derive_ret != true)
        {
            fprintf(stderr, "\nError, derive key fail in [%s].", __FUNCTION__);
            ret = SP_INTERNAL_ERROR;
            break;
        }

        derive_ret = derive_key(&dh_key, SAMPLE_RA_KEY_SK,
                                (uint8_t*)&g_sp_db.sk_key);
        if(derive_ret != true)
        {
            fprintf(stderr, "\nError, derive key fail in [%s].", __FUNCTION__);
            ret = SP_INTERNAL_ERROR;
            break;
        }

        derive_ret = derive_key(&dh_key, SAMPLE_RA_KEY_VK,
                                (uint8_t*)&g_sp_db.vk_key);
        if(derive_ret != true)
        {
            fprintf(stderr, "\nError, derive key fail in [%s].", __FUNCTION__);
            ret = SP_INTERNAL_ERROR;
            break;
        }

        uint32_t msg2_size = sizeof(sample_ra_msg2_t) + sig_rl_size;
        p_msg2_full = (ra_samp_response_header_t*)malloc(msg2_size
                      + sizeof(ra_samp_response_header_t));
        if(!p_msg2_full)
        {
            fprintf(stderr, "\nError, out of memory in [%s].", __FUNCTION__);
            ret = SP_INTERNAL_ERROR;
            break;
        }
        memset(p_msg2_full, 0, msg2_size + sizeof(ra_samp_response_header_t));
		p_msg2_full->type = TYPE_RA_MSG2;
		p_msg2_full->size = msg2_size;
		// @TODO: Set the status properly based on real protocol communication.
		p_msg2_full->status[0] = 0;
		p_msg2_full->status[1] = 0;
		p_msg2 = (sample_ra_msg2_t *)p_msg2_full->body;

        // Assemble MSG2
        if(memcpy_s(&p_msg2->g_b, sizeof(p_msg2->g_b), &g_sp_db.g_b,
                    sizeof(g_sp_db.g_b)) ||
           memcpy_s(&p_msg2->spid, sizeof(sample_spid_t),
                    &g_spid, sizeof(g_spid)))
        {
            fprintf(stderr,"\nError, memcpy failed in [%s].", __FUNCTION__);
            ret = SP_INTERNAL_ERROR;
            break;
        }

        // The service provider is responsible for selecting the proper EPID
        // signature type and to understand the implications of the choice!
        p_msg2->quote_type = SAMPLE_LINKABLE_SIGNATURE;

        // Create gb_ga
        sample_ec_pub_t gb_ga[2];
        if(memcpy_s(&gb_ga[0], sizeof(gb_ga[0]), &g_sp_db.g_b,
                    sizeof(g_sp_db.g_b))
           || memcpy_s(&gb_ga[1], sizeof(gb_ga[1]), &g_sp_db.g_a,
                       sizeof(g_sp_db.g_a)))
        {
            fprintf(stderr,"\nError, memcpy failed in [%s].", __FUNCTION__);
            ret = SP_INTERNAL_ERROR;
            break;
        }

        // Sign gb_ga
        sample_ret = sample_ecdsa_sign((uint8_t *)&gb_ga, sizeof(gb_ga),
                        (sample_ec256_private_t *)&g_sp_priv_key,
                        (sample_ec256_signature_t *)&p_msg2->sign_gb_ga,
                        ecc_state);
        if(SAMPLE_SUCCESS != sample_ret)
        {
            fprintf(stderr, "\nError, sign ga_gb fail in [%s].", __FUNCTION__);
            ret = SP_INTERNAL_ERROR;
            break;
        }

        // Generate the CMACsmk for gb||SPID||TYPE||Sigsp(gb,ga)
        uint8_t mac[SAMPLE_EC_MAC_SIZE] = {0};
        uint32_t cmac_size = offsetof(sample_ra_msg2_t, mac);
		sample_ret = sample_rijndael128_cmac_msg(&g_sp_db.smk_key,
            (uint8_t *)&p_msg2->g_b, cmac_size, &mac);
        if(SAMPLE_SUCCESS != sample_ret)
        {
            fprintf(stderr, "\nError, cmac fail in [%s].", __FUNCTION__);
            ret = SP_INTERNAL_ERROR;
            break;
        }
        if(memcpy_s(&p_msg2->mac, sizeof(p_msg2->mac), mac, sizeof(mac)))
        {
            fprintf(stderr,"\nError, memcpy failed in [%s].", __FUNCTION__);
            ret = SP_INTERNAL_ERROR;
            break;
        }

		if(memcpy_s(&p_msg2->sig_rl[0], sig_rl_size, sig_rl, sig_rl_size))
		{
            fprintf(stderr,"\nError, memcpy failed in [%s].", __FUNCTION__);
            ret = SP_INTERNAL_ERROR;
            break;
        }
        p_msg2->sig_rl_size = sig_rl_size;

    }while(0);

    if(ret)
    {
        *pp_msg2 = NULL;
        SAFE_FREE(p_msg2_full);
    }
    else
    {
        // Freed by the network simulator in ra_free_network_response_buffer
        *pp_msg2 = p_msg2_full;
    }

    if(ecc_state)
    {
        sample_ecc256_close_context(ecc_state);
    }

    return ret;
}

// Process remote attestation message 3
int sp_ra_proc_msg3_req(const sample_ra_msg3_t *p_msg3,
                        uint32_t msg3_size,
                        ra_samp_response_header_t **pp_att_result_msg)
{
    int ret = 0;
    sample_status_t sample_ret = SAMPLE_SUCCESS;
    const uint8_t *p_msg3_cmaced = NULL;
    sample_quote_t *p_quote = NULL;
    sample_sha_state_handle_t sha_handle = NULL;
    sample_report_data_t report_data = {0};
    sample_ra_att_result_msg_t *p_att_result_msg = NULL;
    ra_samp_response_header_t* p_att_result_msg_full = NULL;
    uint32_t i;

    if((!p_msg3) ||
       (msg3_size < sizeof(sample_ra_msg3_t)) ||
       (!pp_att_result_msg))
    {
        return SP_INTERNAL_ERROR;
    }

    do
    {
        // Compare g_a in message 3 with local g_a.
        ret = memcmp(&g_sp_db.g_a, &p_msg3->g_a, sizeof(sample_ec_pub_t));
        if(ret)
        {
            fprintf(stderr, "\nError, g_a is not same [%s].", __FUNCTION__);
            ret = SP_PROTOCOL_ERROR;
            break;
        }
        //Make sure that msg3_size is bigger than sample_mac_t.
        uint32_t mac_size = msg3_size - sizeof(sample_mac_t);
        p_msg3_cmaced = reinterpret_cast<const uint8_t*>(p_msg3);
        p_msg3_cmaced += sizeof(sample_mac_t);

        // Verify the message mac using SMK
        sample_cmac_128bit_tag_t mac = {0};
        sample_ret = sample_rijndael128_cmac_msg(&g_sp_db.smk_key,
                                           p_msg3_cmaced,
                                           mac_size,
                                           &mac);
        if(SAMPLE_SUCCESS != sample_ret)
        {
            fprintf(stderr, "\nError, cmac fail in [%s].", __FUNCTION__);
            ret = SP_INTERNAL_ERROR;
            break;
        }
        // In real implementation, should use a time safe version of memcmp here,
        // in order to avoid side channel attack.
        ret = memcmp(&p_msg3->mac, mac, sizeof(mac));
        if(ret)
        {
            fprintf(stderr, "\nError, verify cmac fail [%s].", __FUNCTION__);
            ret = SP_INTEGRITY_FAILED;
            break;
        }

        if(memcpy_s(&g_sp_db.ps_sec_prop, sizeof(g_sp_db.ps_sec_prop),
            &p_msg3->ps_sec_prop, sizeof(p_msg3->ps_sec_prop)))
        {
            fprintf(stderr,"\nError, memcpy failed in [%s].", __FUNCTION__);
            ret = SP_INTERNAL_ERROR;
            break;
        }

        p_quote = (sample_quote_t *)p_msg3->quote;

        // Verify the the report_data in the Quote matches the expected value.
        // The first 32 bytes of report_data are SHA256 HASH of {ga|gb|vk}.
        // The second 32 bytes of report_data are set to zero.
        sample_ret = sample_sha256_init(&sha_handle);
        if(sample_ret != SAMPLE_SUCCESS)
        {
            fprintf(stderr,"\nError, init hash failed in [%s].", __FUNCTION__);
            ret = SP_INTERNAL_ERROR;
            break;
        }
        sample_ret = sample_sha256_update((uint8_t *)&(g_sp_db.g_a),
                                     sizeof(g_sp_db.g_a), sha_handle);
        if(sample_ret != SAMPLE_SUCCESS)
        {
            fprintf(stderr,"\nError, udpate hash failed in [%s].",
                    __FUNCTION__);
            ret = SP_INTERNAL_ERROR;
            break;
        }
        sample_ret = sample_sha256_update((uint8_t *)&(g_sp_db.g_b),
                                     sizeof(g_sp_db.g_b), sha_handle);
        if(sample_ret != SAMPLE_SUCCESS)
        {
            fprintf(stderr,"\nError, udpate hash failed in [%s].",
                    __FUNCTION__);
            ret = SP_INTERNAL_ERROR;
            break;
        }
        sample_ret = sample_sha256_update((uint8_t *)&(g_sp_db.vk_key),
                                     sizeof(g_sp_db.vk_key), sha_handle);
        if(sample_ret != SAMPLE_SUCCESS)
        {
            fprintf(stderr,"\nError, udpate hash failed in [%s].",
                    __FUNCTION__);
            ret = SP_INTERNAL_ERROR;
            break;
        }
        sample_ret = sample_sha256_get_hash(sha_handle,
                                      (sample_sha256_hash_t *)&report_data);
        if(sample_ret != SAMPLE_SUCCESS)
        {
            fprintf(stderr,"\nError, Get hash failed in [%s].", __FUNCTION__);
            ret = SP_INTERNAL_ERROR;
            break;
        }
        ret = memcmp((uint8_t *)&report_data,
                     (uint8_t *)&(p_quote->report_body.report_data),
                     sizeof(report_data));
        if(ret)
        {
            fprintf(stderr, "\nError, verify hash fail [%s].", __FUNCTION__);
            ret = SP_INTEGRITY_FAILED;
            break;
        }

        // Verify Enclave policy (IAS may provide an API for this if we
        // registered an Enclave policy)

        // Verify quote with IAS.
        // @IAS_Q: What is the proper JSON format for attestation evidence?
        ias_att_report_t attestation_report;
        // @TODO: Convert this call to a 'network' send/receive
        // once the IAS server is a vaialable.
        ret = ias_verify_attestation_evidence(p_quote, NULL,
                                              &attestation_report);
        if(0 != ret)
        {
            ret = SP_IAS_FAILED;
            break;
        }
        FILE* OUTPUT = stdout;
        fprintf(OUTPUT, "\n\n\tAtestation Report:");
        fprintf(OUTPUT, "\n\tid: 0x%0x.", attestation_report.id);
        fprintf(OUTPUT, "\n\tstatus: %d.", attestation_report.status);
        fprintf(OUTPUT, "\n\trevocation_reason: %u.",
                attestation_report.revocation_reason);
        // attestation_report.info_blob;
        fprintf(OUTPUT, "\n\tpse_status: %d.",  attestation_report.pse_status);
        // Check if Platform_Info_Blob is available.
        // @TODO: Currenlty, the IAS spec says this will not be available if
        // no info blob status flags are set. For now, assume it is always
        // there until we have the full message format definition.

        // Respond the client with the results of the attestation.
        uint32_t att_result_msg_size = sizeof(sample_ra_att_result_msg_t)
                                       + attestation_report.policy_report_size;
        p_att_result_msg_full =
            (ra_samp_response_header_t*)malloc(att_result_msg_size
            + sizeof(ra_samp_response_header_t) + sizeof(g_secret));
        if(!p_att_result_msg_full)
        {
            fprintf(stderr, "\nError, out of memory in [%s].", __FUNCTION__);
            ret = SP_INTERNAL_ERROR;
            break;
        }
        memset(p_att_result_msg_full, 0, att_result_msg_size
               + sizeof(ra_samp_response_header_t) + sizeof(g_secret));
        p_att_result_msg_full->type = TYPE_RA_ATT_RESULT;
        p_att_result_msg_full->size = att_result_msg_size;
        if(IAS_QUOTE_OK != attestation_report.status)
        {
            p_att_result_msg_full->status[0] = 0xFF;
        }
        if(IAS_PSE_OK != attestation_report.pse_status)
        {
            p_att_result_msg_full->status[1] = 0xFF;
        }

        p_att_result_msg =
            (sample_ra_att_result_msg_t *)p_att_result_msg_full->body;

        // @TODO: In the product, the HTTP response header itself will have
        // an RK based signature that the service provider needs to check here.

        // The platform_info_blob signature will be verified by the client
        // if needed. No need to have the Service Provider to check it.

        // @TODO: Verify the enlcave policy report if they are to be supported
        // by IAS. Otherwise, the SP will need to check the ISV enclave report
        // itself.
        fprintf(OUTPUT, "\n\n\tEnclave Report:");
        fprintf(OUTPUT, "\n\tSignature Type: 0x%x", p_quote->sign_type);
        fprintf(OUTPUT, "\n\tSignature Basename: ");
        for(i=0; i<sizeof(p_quote->basename.name) && p_quote->basename.name[i];
            i++)
        {
            fprintf(OUTPUT, "%c", p_quote->basename.name[i]);
        }
#ifdef __x86_64__
        fprintf(OUTPUT, "\n\tattributes.flags: 0x%0lx",
                p_quote->report_body.attributes.flags);
        fprintf(OUTPUT, "\n\tattributes.xfrm: 0x%0lx",
                p_quote->report_body.attributes.xfrm);
#else
        fprintf(OUTPUT, "\n\tattributes.flags: 0x%0llx",
                p_quote->report_body.attributes.flags);
        fprintf(OUTPUT, "\n\tattributes.xfrm: 0x%0llx",
                p_quote->report_body.attributes.xfrm);
#endif
        fprintf(OUTPUT, "\n\tmr_enclave: ");
        for(i=0;i<sizeof(sample_measurement_t);i++)
        {

            fprintf(OUTPUT, "%02x",p_quote->report_body.mr_enclave[i]);

            //fprintf(stderr, "%02x",p_quote->report_body.mr_enclave.m[i]);

        }
        fprintf(OUTPUT, "\n\tmr_signer: ");
        for(i=0;i<sizeof(sample_measurement_t);i++)
        {

            fprintf(OUTPUT, "%02x",p_quote->report_body.mr_signer[i]);

            //fprintf(stderr, "%02x",p_quote->report_body.mr_signer.m[i]);

        }
        fprintf(OUTPUT, "\n\tisv_prod_id: 0x%0x",
                p_quote->report_body.isv_prod_id);
        fprintf(OUTPUT, "\n\tisv_svn: 0x%0x",p_quote->report_body.isv_svn);
        fprintf(OUTPUT, "\n");
        // @TODO do a real check here.
        bool isv_policy_passed = true;

        // Assemble Attestation Result Message
        // Note, this is a structure copy.  We don't copy the policy reports
        // right now.
        p_att_result_msg->platform_info_blob = attestation_report.info_blob;

        // Generate mac based on the mk key.
        mac_size = sizeof(ias_platform_info_blob_t);
        sample_ret = sample_rijndael128_cmac_msg(&g_sp_db.mk_key,
            (const uint8_t*)&p_att_result_msg->platform_info_blob,
            mac_size,
            &p_att_result_msg->mac);
        if(SAMPLE_SUCCESS != sample_ret)
        {
            fprintf(stderr, "\nError, cmac fail in [%s].", __FUNCTION__);
            ret = SP_INTERNAL_ERROR;
            break;
        }

        // Generate shared secret and encrypt it with SK, if attestation passed.
        uint8_t aes_gcm_iv[SAMPLE_SP_IV_SIZE] = {0};
        p_att_result_msg->secret.payload_size = 8;
        if((IAS_QUOTE_OK == attestation_report.status) &&
           (IAS_PSE_OK == attestation_report.pse_status) &&
           (isv_policy_passed == true))
        {
            ret = sample_rijndael128GCM_encrypt(&g_sp_db.sk_key,
                        &g_secret[0],
                        p_att_result_msg->secret.payload_size,
                        p_att_result_msg->secret.payload,
                        &aes_gcm_iv[0],
                        SAMPLE_SP_IV_SIZE,
                        NULL,
                        0,
                        &p_att_result_msg->secret.payload_tag);
        }
    }while(0);

    if(ret)
    {
        *pp_att_result_msg = NULL;
        SAFE_FREE(p_att_result_msg_full);
    }
    else
    {
        // Freed by the network simulator in ra_free_network_response_buffer
        *pp_att_result_msg = p_att_result_msg_full;
    }
    return ret;
}



