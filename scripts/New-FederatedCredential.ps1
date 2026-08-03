<#
.SYNOPSIS
    Provisions the Entra federated credential that lets CI sign without a stored
    secret, and switches the repository over to it.

.DESCRIPTION
    CI currently authenticates with a client secret held in the release
    environment. That works, but the secret is a long-lived credential sitting
    in GitHub.

    OIDC removes it: GitHub mints a short-lived token per run and Entra trusts
    it because of a federated credential on the app registration.

    This cannot be done by the signing service principal itself. It has no Graph
    application roles, so it cannot read or modify its own app registration:

        Authorization_RequestDenied: Insufficient privileges

    So this script signs you in INTERACTIVELY as a directory administrator. It
    is a one-time human step, which is exactly why it is a script and not a
    paragraph in a runbook.

.PARAMETER Repository
    owner/name, e.g. Locke-Werks/Forge.

.NOTES
    The subject MUST use the tag form. A federated credential whose subject is
    a branch will not match a tag push, and the failure arrives at sign time as
    a 403 rather than at login, which makes it look like a permissions problem
    with the certificate profile instead of a mismatched subject.
#>

[CmdletBinding()]
param(
    [string]$Repository = "Locke-Werks/Forge",
    [string]$Environment = "release",
    [string]$CredentialName = "github-release-tags"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$RepoRoot = Split-Path -Parent $PSScriptRoot
$envFile = Join-Path $RepoRoot 'signing\signing.env'

$clientId = (gh variable get AZURE_CLIENT_ID --repo $Repository)
$tenantId = (gh variable get AZURE_TENANT_ID --repo $Repository)
if (-not $clientId -or -not $tenantId) {
    throw "AZURE_CLIENT_ID / AZURE_TENANT_ID are not set as repository variables on $Repository"
}

Write-Host "app registration: $clientId"
Write-Host "tenant:           $tenantId"
Write-Host "repository:       $Repository"
Write-Host "signing config:   $envFile"

if (-not (Get-Command az -ErrorAction SilentlyContinue)) {
    throw @"
Azure CLI is not installed. Install it and re-run:

    winget install -e --id Microsoft.AzureCLI

Then open a NEW shell so PATH picks it up.
"@
}

# Interactive, and deliberately so. An admin has to consent to a directory
# change; a service principal cannot grant itself the right to make one.
Write-Host "`nSigning in interactively. Use an account with rights over the app registration." `
           -ForegroundColor Cyan
az login --tenant $tenantId --allow-no-subscriptions | Out-Null

$appObjectId = az ad app show --id $clientId --query id -o tsv
if (-not $appObjectId) { throw "could not resolve the app registration object id" }

# The environment must appear in the subject when the workflow job declares one,
# and the tag form is what a `v*` push produces.
$subject = "repo:${Repository}:environment:${Environment}"

$body = @{
    name        = $CredentialName
    issuer      = "https://token.actions.githubusercontent.com"
    subject     = $subject
    description = "GitHub Actions release signing for $Repository"
    audiences   = @("api://AzureADTokenExchange")
} | ConvertTo-Json -Compress

$existing = az ad app federated-credential list --id $appObjectId --query "[?name=='$CredentialName'].id" -o tsv
if ($existing) {
    Write-Host "`nfederated credential '$CredentialName' already exists; updating subject" -ForegroundColor Yellow
    az ad app federated-credential update --id $appObjectId --federated-credential-id $existing `
        --parameters $body | Out-Null
}
else {
    Write-Host "`ncreating federated credential '$CredentialName'" -ForegroundColor Cyan
    az ad app federated-credential create --id $appObjectId --parameters $body | Out-Null
}

Write-Host "subject: $subject" -ForegroundColor Green

Write-Host @"

Done. To switch CI over, edit .github/workflows/ci.yml:

  1. In the sign job's permissions, add:

         id-token: write

  2. Replace the three azure-* inputs on the trusted-signing-action step with an
     azure/login@v2 step before it:

         - uses: azure/login@v2
           with:
             client-id: `${{ vars.AZURE_CLIENT_ID }}
             tenant-id: `${{ vars.AZURE_TENANT_ID }}
             subscription-id: `${{ vars.AZURE_SUBSCRIPTION_ID }}

  3. Delete the AZURE_CLIENT_SECRET reference from the preflight step, then:

         gh secret delete AZURE_CLIENT_SECRET --repo $Repository --env $Environment

Until step 3 is done the secret is still live, so rotating it in Entra is worth
doing at the same time.
"@ -ForegroundColor Gray
