/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 *
 * The contents of this file are subject to the Netscape Public
 * License Version 1.1 (the "License"); you may not use this file
 * except in compliance with the License. You may obtain a copy of
 * the License at http://www.mozilla.org/NPL/
 *
 * Software distributed under the License is distributed on an "AS
 * IS" basis, WITHOUT WARRANTY OF ANY KIND, either express or
 * implied. See the License for the specific language governing
 * rights and limitations under the License.
 *
 * The Original Code is mozilla.org code.
 *
 * The Initial Developer of the Original Code is Netscape
 * Communications Corporation.  Portions created by Netscape are
 * Copyright (C) 1998 Netscape Communications Corporation. All
 * Rights Reserved.
 *
 * Contributor(s): 
 */
package netscape.ldap.client;

import java.util.Vector;
import java.io.*;
import netscape.ldap.ber.stream.*;

/**
 * This class provides miscellaneous operations for JDAPFilter object.
 * It converts string with escape characters to the byte array. It also
 * returns the ber octet string for the specified string with escape
 * characters.
 */
class JDAPFilterOpers {

    private static final String escapeKey = "\\";
    private static final boolean m_debug = false;

    /**
     * Returns the octetString for the given string
     * @return The octetString for the given string
     */
    static BEROctetString getOctetString(String str) {
        if (str.indexOf(escapeKey) >= 0) {
            byte[] byteVal = JDAPFilterOpers.getByteValues(str);
            return new BEROctetString(byteVal);
        }
        else
            return new BEROctetString(str);
    }

    /**
     * This method converts the given string into bytes. It also handles
     * the escape characters embedded in the given string.
     * @param str The given string being converted into a byte array
     * @return A byte array
     */
    static byte[] getByteValues(String str) {
        int pos = 0;
        Vector v = new Vector();
        String val = new String(str);
        int totalSize = 0;

        // check if any escape character in the string
        while ((pos = val.indexOf(escapeKey)) >= 0) {
            String s1 = val.substring(0, pos);

            try {
                byte[] b = s1.getBytes("UTF8");
                totalSize += b.length;
                v.addElement(b);
            } catch (UnsupportedEncodingException e) {
                printDebug(e.toString());
                return null;
            }

            String hex = "0x"+val.substring(pos+1, pos+3);

            Integer num = null;

            // decode this number to integer, exception thrown when this is not the
            // hex
            try {
                num = Integer.decode(hex);
            } catch (NumberFormatException e) {
                printDebug(e.toString());
                return null;
            }

            byte[] b = {(byte)num.intValue()};
            totalSize += b.length;

            v.addElement(b);

            // skip an escape and two chars after escape
            val = val.substring(pos+3);
         }

        if (val.length() > 0) {
            try {
                byte[] b = val.getBytes("UTF8");
                totalSize += b.length;
                v.addElement(b);
            } catch (UnsupportedEncodingException e) {
                printDebug(e.toString());
                return null;
            }
        }

        byte[] result = new byte[totalSize];
        pos = 0;
        for (int i=0; i<v.size(); i++) {
            byte[] b = (byte[])v.elementAt(i);
            System.arraycopy(b, 0, result, pos, b.length);
            pos = pos+b.length;
        }

        return result;
    }

    /**
     * Print debug message
     */
    private static void printDebug(String str) {
        if (m_debug)
            System.out.println(str);
    }
}

